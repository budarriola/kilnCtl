#include "rules_task.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "MAX31856.h"
#include "heat_interlock.h"
#include "kiln_io.h"
#include "kiln_io_owner.h"
#include "ota_http.h"
#include "thermo_owner.h"
#include "profile_executor.h"
#include "relay_authority.h"
#include "rules_eval.h"
#include "rules_http.h"
#include "stack_margin.h"
#include "thermo_combine.h"
#include "zones_http.h"

static const char *TAG = "rules_task";

/* 1Hz, same cadence as profile_executor.c's control tick -- TEMP thresholds
 * do not need faster, and TIME conditions are specified in whole seconds. */
#define RULES_TASK_TICK_MS 1000u

/* Stale-tick watchdog (the gap rules_task.h's top comment used to flag as
 * "not built here"): if the main tick above hasn't completed in this long,
 * a second, independent task (rules_watchdog_entry() below) force-releases
 * every relay this task claimed and drives them OFF directly, rather than
 * leaving them at whatever they were last commanded to -- the same
 * "leaving a relay stuck at its last state is not a fail-safe" reasoning
 * every other path in this file already follows for the safety link and the
 * OTA interlock. 5x the tick period gives a wedged/slow tick a few misses'
 * worth of slack before tripping (a single missed tick under normal
 * scheduling jitter must not false-trigger a live kiln's relays off). */
#define RULES_WATCHDOG_STALE_MS (RULES_TASK_TICK_MS * 5u)
#define RULES_WATCHDOG_POLL_MS 1000u

_Static_assert(RULES_EVAL_RELAY_COUNT == KILN_IO_RELAY_COUNT,
               "rules_types.h's RULES_EVAL_RELAY_COUNT drifted from kiln_io.h's KILN_IO_RELAY_COUNT");
_Static_assert(RULES_EVAL_ZONE_COUNT == MAX31856_CHANNEL_COUNT,
               "rules_types.h's RULES_EVAL_ZONE_COUNT drifted from MAX31856.h's MAX31856_CHANNEL_COUNT");

static struct {
    SafetyLinkClass *safety; /* NULL if not wired -- every ON decision fails closed, see rules_task.h */
    TaskHandle_t task;
    TaskHandle_t watchdog_task;

    /* Bits this task itself currently holds as RELAY_OWNER_RULE -- only
     * these bits are ever released by this task (see sync_ownership()'s
     * doc comment: never release a relay this task didn't claim). Also
     * read (not written) by rules_watchdog_entry() on a stale-tick trip --
     * see that function's comment for the accepted race with the main task
     * resuming at the same instant. */
    uint8_t claimed_mask;

    /* esp_timer_get_time() timestamp (us) of the last completed main-task
     * tick -- written every iteration by rules_task_entry(), read by
     * rules_watchdog_entry() to detect a wedged/dead tick. Plain int64_t,
     * not atomic: a torn read here is at worst one watchdog poll early or
     * late, never a correctness issue (matches this struct's existing
     * lock-free tolerance). */
    volatile int64_t last_tick_us;

    /* Published status, read by rules_http.c's GET /api/rules/status.
     * Written only by the task below; read-only snapshot copy handed out
     * by rules_task_get_status() -- no lock, same tolerance as
     * rules_http_get_cfg() (worst case: one stale/torn read, self-corrects
     * next tick, matches this codebase's existing risk posture for
     * dashboard-poll-only state). */
    rules_task_status_t status;
} s_rules_task;

/* Union of every configured zone's PID/heater relay mask -- bit N-1 = relay
 * N belongs to SOME zone and is therefore under PID/thermocouple control.
 * Recomputed fresh EVERY tick, not cached at startup: zones_http.c's zone
 * config (relay_mask per zone) is editable at runtime from the Thermocouples
 * & Zones page, so a relay can be assigned to (or removed from) a zone at
 * any time while this task is already running. Caching this once at
 * rules_task_start() would let an operator move a relay into a zone mid-run
 * and have the rule engine keep driving it as if nothing changed -- exactly
 * the hazard the owner's design rule exists to close. The cost of
 * recomputing it every second is a handful of zones_config_get_relay_mask()
 * calls (pure struct reads, no I/O), negligible next to this task's own
 * kiln_io_owner round trip. */
static uint8_t compute_heater_relay_mask(void)
{
    uint8_t mask = 0;
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (zones_config_get_relay_mask(zi, &zone_mask)) {
            mask |= zone_mask;
        }
    }
    return mask;
}

/* Only ever touches bits THIS task previously claimed (s_rules_task.claimed_
 * mask) when releasing, and only claims a relay that is currently unowned
 * or already ours -- a relay a running profile or autotune test owns first
 * is left alone and logged, not stolen. See rules_task.h's top comment for
 * the full ownership-precedence writeup this implements. */
static void sync_ownership(uint8_t want_rule_driven_mask)
{
    uint8_t to_release = (uint8_t)(s_rules_task.claimed_mask & (uint8_t)~want_rule_driven_mask);
    if (to_release) {
        relay_authority_release_mask(to_release);
        s_rules_task.claimed_mask = (uint8_t)(s_rules_task.claimed_mask & (uint8_t)~to_release);
    }

    uint8_t to_claim = (uint8_t)(want_rule_driven_mask & (uint8_t)~s_rules_task.claimed_mask);
    for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
        uint8_t bit = (uint8_t)(1u << (ri - 1u));
        if ((to_claim & bit) == 0) {
            continue;
        }
        relay_owner_t owner = relay_authority_get_owner(ri);
        if (owner == RELAY_OWNER_NONE || owner == RELAY_OWNER_RULE) {
            relay_authority_claim_mask(bit, RELAY_OWNER_RULE);
            s_rules_task.claimed_mask |= bit;
        } else {
            ESP_LOGW(TAG,
                     "relay %u marked rule_driven but already owned by owner=%d -- rule engine will "
                     "not drive it until that owner releases it",
                     (unsigned)ri, (int)owner);
        }
    }
}

static void gather_inputs(rules_eval_inputs_t *in, bool *out_relay_commanded_on /* [RULES_EVAL_RELAY_COUNT] */)
{
    memset(in, 0, sizeof(*in));

    profile_exec_status_t exec;
    profile_executor_get_status(&exec);
    in->profile_running = (exec.state == PROFILE_EXEC_RUNNING);
    in->profile_elapsed_s = exec.total_elapsed_s;

    /* Zone temperatures. The executor is the preferred source while a firing
     * is under way -- it is already combining each zone's thermocouples the
     * same way the control loop sees them, so a rule and the PID loop agree
     * on what "zone 0" reads.
     *
     * But the executor only populates zones[] while it is RUNNING, and rules
     * exist to drive vents, blowers and reduction flame -- hardware whose
     * whole point may be a cooling kiln with no profile running. Sourcing
     * temperature ONLY from the executor made every TEMP condition
     * permanently false when idle: the rule could never fire, the page showed
     * "Firing: no" with no reason, and the thermocouples were reading
     * perfectly the whole time. Found while testing the live board with a
     * zone at 32C and a rule of "zone 0 >= 20C" that never fired.
     *
     * So: fall back to the live thermocouple readings, combined per zone with
     * the same thermo_combine() the executor itself uses (via each zone's
     * configured thermo_mask), whenever the executor has nothing for a zone.
     * A zone with no valid thermocouple still reads invalid, and an invalid
     * zone still fails its condition closed -- the fail-safe direction is
     * unchanged. */
    /* Live thermocouple readings, via thermo_owner's queue.
     *
     * NOT dashboard_get_status(): that assembles the whole /api/status
     * picture (every channel plus the safety/link block) and needs more stack
     * than this 3072-byte task has. Calling it here overflowed the stack and
     * left the board crash-looping -- "***ERROR*** A stack overflow in task
     * rules_task", five boots in a row. thermo_owner_command_read_all() is
     * the narrow read this actually needs.
     *
     * Calibration is applied with the same zones_config_apply_cal() the
     * dashboard and the control loop use, so a rule threshold means the same
     * temperature the operator reads on the page. Comparing an operator's
     * "open the vent at 200C" against an uncalibrated number would be a
     * quiet, dangerous disagreement. */
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t n_read = 0;
    memset(readings, 0, sizeof(readings));
    (void)thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &n_read);

    float ch_c[MAX31856_CHANNEL_COUNT];
    bool ch_ok[MAX31856_CHANNEL_COUNT];
    for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
        /* Freshness judged by age_ms against KILN_TEMP_STALE_AGE_MS, the same
         * user-facing rule MAX31856.h documents -- NOT by ::stale, which only
         * means "no new conversion since the last poll" and is true for a
         * perfectly good reading taken 200ms ago. */
        bool have = (ci < n_read) && !readings[ci].spi_failed &&
                    !isnan(readings[ci].tc_temperature_c) &&
                    readings[ci].age_ms != MAX31856_READING_AGE_UNKNOWN &&
                    readings[ci].age_ms < KILN_TEMP_STALE_AGE_MS;
        ch_c[ci] = have ? zones_config_apply_cal(ci, readings[ci].tc_temperature_c) : 0.0f;
        ch_ok[ci] = have;
    }

    /* Per-zone temperature. The executor is preferred while a firing is under
     * way -- it is already combining each zone's thermocouples exactly as the
     * control loop sees them, so a rule and the PID agree on what "zone 0"
     * reads. Otherwise combine the live readings ourselves with the same
     * thermo_combine() and the zone's configured thermo_mask.
     *
     * The fallback is the whole point: rules drive vents, blowers and
     * reduction flame, and those may well need to act on a cooling kiln with
     * no profile running. Sourcing zone temperature ONLY from the executor
     * (as this did originally) left every TEMP condition false whenever idle
     * -- the rule simply never fired, with the page showing "Firing: no" and
     * no hint why, while the thermocouples read perfectly the whole time. */
    for (uint8_t zi = 0; zi < RULES_EVAL_ZONE_COUNT; zi++) {
        if (exec.zones[zi].active && exec.zones[zi].actual_valid) {
            in->zone_temp_valid[zi] = true;
            in->zone_temp_c[zi] = exec.zones[zi].actual_c;
            continue;
        }
        uint8_t thermo_mask = 0;
        bool combined_valid = false;
        float combined_c = 0.0f;
        if (zi < zones_config_get_thermo_count() && zones_config_get_thermo_mask(zi, &thermo_mask)) {
            combined_c = thermo_combine(ch_c, ch_ok, MAX31856_CHANNEL_COUNT, thermo_mask, &combined_valid);
        }
        in->zone_temp_valid[zi] = combined_valid;
        in->zone_temp_c[zi] = combined_valid ? combined_c : 0.0f;
    }

    /* Current commanded relay state, for COND_RELAY conditions. A read
     * failure (owner task down/timeout) leaves relay_shadow at 0 (zeroed
     * above) -- COND_RELAY conditions then read every relay as OFF, which
     * cannot cause a false "other relay is on" match that would help turn
     * something ON; the worst case is a rule that should fire (because the
     * other relay really is on) failing to, which is the fail-safe
     * direction. */
    kiln_io_state_t state;
    memset(&state, 0, sizeof(state));
    esp_err_t err = kiln_io_owner_command_read(&state);
    uint8_t relay_shadow = (err == ESP_OK) ? state.relay_shadow : 0;
    for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
        bool on = (relay_shadow & (1u << (ri - 1u))) != 0;
        in->relay_commanded_on[ri - 1] = on;
        if (out_relay_commanded_on) {
            out_relay_commanded_on[ri - 1] = on;
        }
    }
}

static void rules_task_entry(void *arg)
{
    (void)arg;

    for (;;) {
        rules_cfg_t cfg;
        rules_http_get_cfg(&cfg);

        /* Recomputed every tick -- see compute_heater_relay_mask()'s doc
         * comment for why this cannot be cached once at startup. */
        uint8_t heater_relay_mask = compute_heater_relay_mask();

        uint8_t want_rule_driven_mask = 0;
        for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
            uint8_t bit = (uint8_t)(1u << (ri - 1u));
            if (cfg.relays[ri - 1].rule_driven && (heater_relay_mask & bit) == 0) {
                /* A heater relay's rule_driven flag is never honored, even
                 * if it is still set in the saved config -- this is the
                 * "assigned to a zone AFTER being marked rule_driven"
                 * ordering hazard from rules_task.h's top comment: the
                 * stale bit is simply never claimed/acted on again, not
                 * rewritten in rules_http.c's stored config (that stays
                 * this task's read-only input, per rules_http_get_cfg()'s
                 * own contract -- only the operator, via a fresh POST
                 * /api/rules, or rules_http.c's own validation on a future
                 * save, ever changes what's on flash). */
                want_rule_driven_mask |= bit;
            }
        }
        sync_ownership(want_rule_driven_mask);

        rules_eval_inputs_t in;
        bool relay_commanded_on[RULES_EVAL_RELAY_COUNT];
        gather_inputs(&in, relay_commanded_on);

        /* Both fail-safe gates, sampled once per tick, same functions
         * kiln_io_owner.c's own relay_on_blocked() combines -- see this
         * file's header comment for why this task must apply them itself
         * before calling the AUTHORIZED producer below. */
        bool safety_link_ok = !relay_authority_on_blocked(s_rules_task.safety, NULL);
        char interlock_reason[HEAT_INTERLOCK_REASON_MAX];
        bool heat_interlock_ok = !ota_http_heat_blocked_by_update(interlock_reason, sizeof(interlock_reason));

        uint8_t want_on_mask = 0;
        uint8_t decided_on_mask = 0;
        for (uint8_t ri = 1; ri <= RULES_EVAL_RELAY_COUNT; ri++) {
            uint8_t bit = (uint8_t)(1u << (ri - 1u));
            const relay_rules_cfg_t *rc = &cfg.relays[ri - 1];
            bool is_heater_relay = (heater_relay_mask & bit) != 0;

            bool wants_on = rules_eval_relay_wants_on(rc, &in);
            bool decided_on = rules_eval_decide(rc, &in, safety_link_ok, heat_interlock_ok, is_heater_relay);

            if (wants_on) {
                want_on_mask |= bit;
            }

            /* Only this task's own RULE-owned relays are ever commanded --
             * a relay marked rule_driven but refused ownership by
             * sync_ownership() above (owned by PROFILE/AUTOTUNE, or a
             * heater relay this task never even attempted to claim) is
             * left strictly alone: not commanded on, not forced off, since
             * this task has no authority over it at all. */
            bool ours = (s_rules_task.claimed_mask & bit) != 0;
            if (ours && decided_on) {
                decided_on_mask |= bit;
            }

            s_rules_task.status.relays[ri - 1].rule_driven = rc->rule_driven;
            s_rules_task.status.relays[ri - 1].rule_wants_on = wants_on;
            s_rules_task.status.relays[ri - 1].owned_by_rules = ours;
            s_rules_task.status.relays[ri - 1].commanded_on = ours && decided_on;
            s_rules_task.status.relays[ri - 1].heater_owned = is_heater_relay;
        }
        s_rules_task.status.safety_link_ok = safety_link_ok;
        s_rules_task.status.heat_interlock_ok = heat_interlock_ok;

        if (s_rules_task.claimed_mask != 0) {
            /* AUTHORIZED producer -- this task already applied its own
             * ownership + safety-fault + OTA-interlock checks above, same
             * justification profile_executor.c's apply_relay() gives for
             * using this entry point instead of the MANUAL one. Only the
             * bits this task owns are ever named in the mask/value pair, so
             * a relay owned by someone else is untouched by this call. */
            esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(s_rules_task.claimed_mask,
                                                                             decided_on_mask);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "kiln_io_owner_command_set_relay_mask_authorized failed: %s",
                         esp_err_to_name(err));
            }
        }

        /* Proves to rules_watchdog_entry() that this tick is alive. Written
         * last, after every other side effect this iteration performs, so a
         * hang anywhere above (including inside the kiln_io_owner call)
         * shows up as a stale timestamp rather than a falsely-fresh one. */
        s_rules_task.status.watchdog_forced_off = false;
        s_rules_task.last_tick_us = esp_timer_get_time();

        vTaskDelay(pdMS_TO_TICKS(RULES_TASK_TICK_MS));
    }
}

/* Independent watchdog task -- see rules_task.h's top comment ("This task's
 * own tick stalls or the task dies") for the fail-safe gap this closes.
 * Polls s_rules_task.last_tick_us; once it has been more than
 * RULES_WATCHDOG_STALE_MS since the main tick last completed, force-drives
 * every relay this task holds RELAY_OWNER_RULE for OFF and releases that
 * ownership, so a wedged main tick cannot leave a relay stuck at its last
 * commanded state indefinitely.
 *
 * Race accepted: if the main task resumes ticking in the exact instant this
 * fires, both tasks may touch s_rules_task.claimed_mask/relay_authority in
 * the same window. Worst case is one extra release/re-claim cycle -- the
 * main tick's own next iteration re-derives everything from scratch (cfg,
 * heater mask, ownership) and self-corrects, same tolerance this file
 * already accepts for a torn rules_http_get_cfg() read. This is strictly
 * better than the prior behavior (no watchdog at all, relay simply frozen),
 * which is the bar this code has to clear. */
static void rules_watchdog_entry(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RULES_WATCHDOG_POLL_MS));

        int64_t last = s_rules_task.last_tick_us;
        int64_t now = esp_timer_get_time();
        int64_t age_ms = (now - last) / 1000;

        if (age_ms < (int64_t)RULES_WATCHDOG_STALE_MS) {
            continue; /* main tick is healthy */
        }

        uint8_t stuck_mask = s_rules_task.claimed_mask;
        if (stuck_mask == 0) {
            continue; /* nothing this task holds ownership of -- nothing to force off */
        }

        ESP_LOGE(TAG,
                 "rules_task has not ticked in %lld ms (claimed_mask=0x%02x) -- forcing its "
                 "relays OFF and releasing RULE ownership",
                 (long long)age_ms, (unsigned)stuck_mask);

        /* Same AUTHORIZED entry point the main tick uses -- this watchdog
         * has the same standing to drive a RULE-owned relay the wedged main
         * tick would have had. Mask/value both 0 outside stuck_mask's bits
         * is unnecessary here since only stuck_mask is ever named. */
        esp_err_t err = kiln_io_owner_command_set_relay_mask_authorized(stuck_mask, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "watchdog force-off failed: %s -- relay(s) may still be commanded ON",
                     esp_err_to_name(err));
        }
        relay_authority_release_mask(stuck_mask);
        s_rules_task.claimed_mask = (uint8_t)(s_rules_task.claimed_mask & (uint8_t)~stuck_mask);
        s_rules_task.status.watchdog_forced_off = true;
    }
}

esp_err_t rules_task_start(SafetyLinkClass *safety)
{
    memset(&s_rules_task, 0, sizeof(s_rules_task));
    s_rules_task.safety = safety;
    s_rules_task.last_tick_us = esp_timer_get_time();

    /* 3072 -> 4096, 2026-08-24, on a MEASUREMENT rather than a guess -- the
     * first real uxTaskGetStackHighWaterMark() reading this project has ever
     * taken (stack_margin.h, TODO.md section 13) reported:
     *
     *   rules_task  hwm=336B of 3072B  10.9% headroom  CRITICAL
     *
     * i.e. 2736 bytes of this stack had already been touched, on an idle
     * board, leaving 336. Every other instrumented task sat between 47% and
     * 82%; this one was the outlier by a wide margin, and it is the rule
     * evaluator that gates heating -- a FreeRTOS stack overflow here corrupts
     * kernel state rather than failing cleanly.
     *
     * It was invisible until the same session fixed a units bug in the
     * reporting itself: ESP-IDF's uxTaskGetStackHighWaterMark() returns BYTES,
     * not the words vanilla FreeRTOS documents, so an inherited *4 was
     * reporting this task at 1392B / 45.3% / OK. The over-reporting direction
     * is the dangerous one, and this is exactly what it was concealing.
     *
     * 4096 leaves 1360B free at the observed worst point, ~33%. Chosen from
     * the measurement rather than doubled for comfort, because this costs
     * 1024 bytes of INTERNAL DRAM on a board whose end-of-boot trough is
     * already below the documented HTTP-socket-failure figure (dram_margin.h)
     * -- the trade is worth it here and would not be everywhere.
     *
     * STILL TO DO: a high-water mark is only as good as the worst path the
     * task has actually taken. This reading came from a board that has never
     * run a real firing, so 2736B is a FLOOR on true usage, not the peak.
     * Re-read this after a profile run with rules configured before treating
     * 33% as settled. Moving this stack to PSRAM would cost no internal DRAM
     * at all, but needs an audit that nothing rules_task calls writes NVS or
     * flash first -- a flash write from a PSRAM-stack task asserts inside
     * ESP-IDF's cache-disable path (see safety_cfg_store.c's deferred flush
     * for that incident). rules_task.c itself makes no nvs_* call; its callees
     * are unaudited. */
    BaseType_t ok = xTaskCreatePinnedToCore(rules_task_entry, "rules_task", 4096, NULL, 4,
                                            &s_rules_task.task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rules_task) failed");
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("rules_task", &s_rules_task.task, 4096);

    /* Independent task (own stack, own priority) so a wedged main tick
     * cannot also wedge its own watchdog -- same reasoning
     * profile_executor.c's guard-9 pattern gives for a second task rather
     * than a self-check inline in the loop that can hang. Lower priority
     * than the main task (3 vs 4): this is a background safety check, not
     * something that should preempt real relay/IO work. */
    ok = xTaskCreatePinnedToCore(rules_watchdog_entry, "rules_watchdog", 2048, NULL, 3,
                                 &s_rules_task.watchdog_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(rules_watchdog) failed -- rules_task has no stale-tick watchdog");
        /* Not fatal to the rule engine itself -- the main task still runs
         * and still fails safe on the safety-link/OTA/heater gates; it just
         * loses the "wedged tick" cover. Report success anyway so a boot
         * doesn't fail outright over a watchdog task alone; the error above
         * is visible in the log for whoever is debugging a low-memory boot. */
    } else {
        /* Same TODO.md section 13 candidate list as rules_task above. */
        stack_margin_register("rules_watchdog", &s_rules_task.watchdog_task, 2048);
    }

    ESP_LOGI(TAG, "rule evaluator task up (safety=%p)", (void *)safety);
    return ESP_OK;
}

void rules_task_get_status(rules_task_status_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_rules_task.status;
}
