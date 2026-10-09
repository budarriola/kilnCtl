#include "telemetry_log.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h" /* MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT below -- was previously reached
                            * only transitively via freertos/idf_additions.h */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "autotune_engine.h"
#include "dram_watch.h" /* dram_watch_service() -- SK-04 log lines, run here, not in the esp_timer callback */
#include "event_log.h"
#include "profile_executor.h"
#include "stack_margin.h"
#include "telemetry_format.h"

static const char *TAG = "KTEL";

/* 1 Hz would match profile_executor's own control tick, but the values it
 * carries (temperature, duty) do not move meaningfully faster than a few
 * seconds on a kiln -- see pid_autotune.h's own tau_s scale (thousands of
 * seconds). 5 s keeps a 30+ minute, 3-zone firing's telemetry volume small
 * while still being far faster than anything a human or a plotting script
 * needs to see a ramp/dwell move.
 *
 * Rate arithmetic (report requirement): a 3-zone FIRE line runs ~190 bytes
 * (measured against telemetry_format_firing()'s actual format string: ~62B
 * header + 3 * ~42B per zone). At one line per 5s that is ~38 B/s, ~2.3
 * KB/min, ~68 KB over a 30-minute firing -- trivially small for a PC to
 * capture, and small relative to uart_log_bridge.c's queue: at a steady
 * 1-per-5s production rate against a link that drains far faster than 5s
 * per line under any real connection, telemetry never has more than about
 * one entry resident in that 64-entry queue at a time (~1.6% occupancy),
 * leaving the other ~63 slots for boot logs and everything else exactly as
 * before this feature existed. */
#define TELEMETRY_LOG_FIRING_PERIOD_S 5u

/* Matches AUTOTUNE_ENGINE_SAMPLE_PERIOD_S (autotune_engine.h) exactly -- the
 * trace itself only gains a new sample every 10s, so telemetry faster than
 * that would just repeat the same reading.
 *
 * Rate arithmetic: a mid-run TUNE line (no model yet) is ~55 bytes; the
 * single DONE line with the full fitted model is ~230 bytes -- both well
 * under UART_LOG_TEXT_MAX (uart_log_bridge.c, ~252B). At one line per 10s
 * for the full 4h AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S budget that is at
 * most 1440 lines * ~55B = ~79 KB, plus one ~230B DONE/ABORTED line --
 * again trivial for a PC to capture, and (same reasoning as FIRE above)
 * never more than about one queue entry at a time in steady state. */
#define TELEMETRY_LOG_AUTOTUNE_PERIOD_S 10u

/* The task's own poll tick -- must divide both periods above so neither
 * drifts. 1 Hz, same as profile_executor's control tick, so "how promptly
 * does telemetry notice DONE/ABORTED" is bounded the same way every other
 * 1 Hz status consumer already is. */
#define TELEMETRY_LOG_TICK_MS 1000u

/* Scratch line buffer. UART_LOG_TEXT_MAX (uart_log_bridge.c) is
 * UART_PROTO_MAX_PAYLOAD-1, currently 252 bytes -- the hard ceiling for what
 * a single queue entry can carry. This is sized well above the worst case
 * either formatter produces in the common (<=3 zone) case specifically so a
 * real call never exercises the formatters' own truncation path;
 * ESP_LOGI/vsnprintf inside uart_log_bridge.c truncates independently and
 * safely if a future field addition ever does push a line past
 * UART_LOG_TEXT_MAX, but that would show up as a truncated telemetry line
 * on the PC -- see telemetry_format.c's own worst-case comment on
 * telemetry_format_firing() for why 5-zone-active is flagged, not solved. */
#define TELEMETRY_LOG_LINE_BUF 320

static volatile bool s_enabled = false;
static TaskHandle_t s_task = NULL;

void telemetry_log_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "telemetry %s", enabled ? "enabled" : "disabled");
}

bool telemetry_log_is_enabled(void)
{
    return s_enabled;
}

/* event_log_code_t for a profile_exec_state_t transition, or -1 if this
 * transition is not one of the events flash persists. Pure decision table,
 * pulled out of the task loop so it is easy to see every transition this
 * file treats as "genuinely necessary for debug" at a glance -- see
 * event_log.h's file banner for the owner decision this implements. */
static int firing_event_code_for_transition(profile_exec_state_t prev, profile_exec_state_t cur)
{
    if (prev == cur) {
        return -1;
    }
    if (cur == PROFILE_EXEC_RUNNING && prev != PROFILE_EXEC_PAUSED) {
        return EVENT_CODE_FIRING_STARTED;
    }
    if (cur == PROFILE_EXEC_PAUSED) {
        return EVENT_CODE_FIRING_PAUSED;
    }
    if (cur == PROFILE_EXEC_RUNNING && prev == PROFILE_EXEC_PAUSED) {
        return EVENT_CODE_FIRING_RESUMED;
    }
    if (cur == PROFILE_EXEC_DONE) {
        return EVENT_CODE_FIRING_DONE;
    }
    if (cur == PROFILE_EXEC_FAULTED) {
        return EVENT_CODE_FIRING_FAULTED;
    }
    return -1;
}

static int autotune_event_code_for_transition(autotune_engine_state_t prev, autotune_engine_state_t cur)
{
    if (prev == cur) {
        return -1;
    }
    if (cur != AUTOTUNE_ENGINE_IDLE && prev == AUTOTUNE_ENGINE_IDLE) {
        return EVENT_CODE_TUNE_STARTED;
    }
    if (cur == AUTOTUNE_ENGINE_DONE) {
        return EVENT_CODE_TUNE_DONE;
    }
    if (cur == AUTOTUNE_ENGINE_ABORTED) {
        return EVENT_CODE_TUNE_ABORTED;
    }
    return -1;
}

static void telemetry_log_task(void *arg)
{
    (void)arg;
    /* telemetry_log_task's own stack is 6144 B (xTaskCreatePinnedToCoreWithCaps
     * below) and this task runs forever, so a 1464-byte profile_exec_status_t
     * still must not live on this stack -- but a heap allocation made exactly
     * once per task lifetime and never freed is functionally the same as a
     * .bss static, and the heap-alloc shape had a real failure mode: on OOM
     * the task logged and deleted itself, silently ending firing telemetry
     * forever with no other symptom. Since fst is used for the task's entire
     * life and never released, own it as a static instead -- .dram0.bss
     * +1464 B, but the OOM-and-vanish failure mode is gone entirely rather
     * than just retried. */
    static profile_exec_status_t fst_storage;
    profile_exec_status_t *fst = &fst_storage;
    autotune_engine_status_t ast;
    char line[TELEMETRY_LOG_LINE_BUF];

    uint32_t firing_next_s = 0;
    uint32_t autotune_next_s = 0;
    profile_exec_state_t firing_prev_state = PROFILE_EXEC_IDLE;
    autotune_engine_state_t autotune_prev_state = AUTOTUNE_ENGINE_IDLE;
    /* PID_EXPANSION_PLAN.md sec 7.1/7.4: per-zone sustained-lag edge
     * tracking. lag_prev_* hold the LAST tick's values while sustained was
     * true, so a CLEARED event (fired the tick ramp_lag_sustained flips
     * back to false, at which point profile_exec_zone_status_t's own
     * held_s/rate fields already read 0 -- see ramp_lag_event_for_
     * transition()'s doc comment) still has real numbers to report. */
    bool lag_prev_sustained[MAX31856_CHANNEL_COUNT] = {0};
    float lag_prev_held_s[MAX31856_CHANNEL_COUNT] = {0};
    float lag_prev_commanded[MAX31856_CHANNEL_COUNT] = {0};
    float lag_prev_achieved[MAX31856_CHANNEL_COUNT] = {0};

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_LOG_TICK_MS));

        /* SK-04 (dram_watch.h): the 2 s esp_timer sampler only records; its
         * "new low" line and the once-per-boot alarm heap dump are logged
         * here, on this task's 6144 B stack, instead of on the shared
         * esp_timer task. No flash access, so the PSRAM-stack note in
         * telemetry_log_start() still holds. */
        dram_watch_service();

        /* The live debug-UART feed (ESP_LOGI below) stays exactly as it
         * was: default-OFF, opt-in via telemetry_log_set_enabled(), the
         * intended home for per-tick temperature/telemetry debugging (owner
         * decision, 2026-09-02: "loging of temps for debug should be done
         * over the uart interface").
         *
         * What changed: flash persistence (event_log_emit(), event_log.h)
         * no longer runs on this 5s/10s poll at all -- it fires ONLY on a
         * genuine state transition (run started/paused/resumed/done/
         * faulted; tune started/done/aborted), as a small binary record,
         * never a per-tick sample. See log_store.h's file banner for why:
         * the old unconditional-every-tick write filled the 256 KiB/kind
         * flash cap in under 2 hours of a single firing.
         *
         * Never blocks: profile_executor_get_status()/
         * autotune_engine_get_status() are documented snapshot copies that
         * never block on their owning task (profile_executor.h); the
         * ESP_LOGI() calls enqueue onto uart_log_bridge.c's queue with a
         * ZERO-tick xQueueSend (see this file's header comment); and
         * event_log_emit() blocks only on the internal-stack flash worker
         * (log_store_mount.c), never on this task's own PSRAM stack
         * touching flash directly. Nothing in this loop can stall waiting
         * on another task indefinitely. */
        profile_executor_get_status(fst);
        if (fst->state == PROFILE_EXEC_RUNNING || fst->state == PROFILE_EXEC_PAUSED) {
            if (fst->total_elapsed_s >= firing_next_s) {
                firing_next_s = fst->total_elapsed_s + TELEMETRY_LOG_FIRING_PERIOD_S;
                if (s_enabled) {
                    int n = telemetry_format_firing(fst, line, sizeof(line));
                    if (n > 0) {
                        ESP_LOGI(TAG, "%s", line);
                    }
                }
            }
        } else {
            firing_next_s = 0; /* re-arm so the NEXT run logs its first tick immediately */
        }
        int firing_code = firing_event_code_for_transition(firing_prev_state, fst->state);
        if (firing_code >= 0) {
            event_log_severity_t sev = (firing_code == EVENT_CODE_FIRING_FAULTED) ? EVENT_LOG_SEV_ERROR
                                                                                   : EVENT_LOG_SEV_INFO;
            int32_t arg = (firing_code == EVENT_CODE_FIRING_FAULTED) ? (int32_t)fst->fault_guard
                                                                      : (int32_t)fst->total_elapsed_s;
            /* PID_EXPANSION_PLAN.md sec 7.3, DEFECT 4 fix: DONE is the one
             * transition where this run's dwell was possibly shortened by
             * ramp-assist dwell credit (profile_exec_status_t.ramp_dwell_
             * credit_applied_s, profile_executor.h) -- the single number
             * saying how many seconds were cut from this firing's dwell(s).
             * Carried in `note` rather than `arg` (arg is already
             * total_elapsed_s for DONE / fault_guard for FAULTED) so this
             * artifact survives in the flash event log even though /api/
             * profile_exec's own live field resets to 0 at the next run
             * start (profile_executor_run.c). Not emitted for FAULTED: a
             * faulted run's dwell-credit figure is not the useful number at
             * that transition (fault_guard is), and it is still visible
             * live via /api/profile_exec's ramp_dwell_credit_applied_s and
             * the last_run breadcrumb until the next firing starts. */
            char firing_note[EVENT_LOG_NOTE_LEN];
            firing_note[0] = '\0';
            const char *note_ptr = NULL;
            if (firing_code == EVENT_CODE_FIRING_DONE && fst->ramp_dwell_credit_applied_s > 0.0f) {
                snprintf(firing_note, sizeof(firing_note), "dwc=%us",
                         (unsigned)fst->ramp_dwell_credit_applied_s);
                note_ptr = firing_note;
            }
            event_log_emit(LOG_STORE_KIND_FIRING, sev, EVENT_LOG_SRC_FIRING, (event_log_code_t)firing_code,
                            EVENT_LOG_ZONE_NONE, arg, note_ptr);
        }
        firing_prev_state = fst->state;

        /* PID_EXPANSION_PLAN.md sec 7.1/7.4: sustained-lag warning event,
         * ALWAYS checked (not gated on ramp_assist_enabled -- see
         * profile_exec_zone_status_t.ramp_lag_sustained's own doc
         * comment). Reset to "not lagging" whenever the run itself is not
         * live, same reasoning as firing_next_s's re-arm just above --
         * without this, a zone that was still mid-lag when a run ended
         * would report a spurious CLEARED (or never report the CLEARED for
         * a lag that was genuinely still open) against the NEXT run's own
         * numbers. */
        bool firing_live = (fst->state == PROFILE_EXEC_RUNNING || fst->state == PROFILE_EXEC_PAUSED);
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            bool cur_sustained = firing_live && fst->zones[zi].active && fst->zones[zi].ramp_lag_sustained;
            int32_t arg = 0;
            uint8_t note[EVENT_LOG_NOTE_LEN];
            int lag_code = telemetry_ramp_lag_event_for_transition(
                lag_prev_sustained[zi], cur_sustained, fst->zones[zi].actual_c,
                fst->zones[zi].ramp_lag_commanded_rate_c_per_hr, fst->zones[zi].ramp_lag_achieved_rate_c_per_hr,
                lag_prev_held_s[zi], lag_prev_commanded[zi], lag_prev_achieved[zi], &arg, note);
            if (lag_code >= 0) {
                event_log_emit(LOG_STORE_KIND_FIRING, EVENT_LOG_SEV_WARN, EVENT_LOG_SRC_FIRING,
                                (event_log_code_t)lag_code, zi, arg, (const char *)note);
            }
            lag_prev_sustained[zi] = cur_sustained;
            if (cur_sustained) {
                lag_prev_held_s[zi] = fst->zones[zi].ramp_lag_held_s;
                lag_prev_commanded[zi] = fst->zones[zi].ramp_lag_commanded_rate_c_per_hr;
                lag_prev_achieved[zi] = fst->zones[zi].ramp_lag_achieved_rate_c_per_hr;
            }
        }

        autotune_engine_get_status(&ast);
        bool at_active = (ast.state != AUTOTUNE_ENGINE_IDLE);
        if (at_active) {
            bool boundary = (ast.state == AUTOTUNE_ENGINE_DONE || ast.state == AUTOTUNE_ENGINE_ABORTED);
            if (boundary || ast.elapsed_s >= autotune_next_s) {
                autotune_next_s = ast.elapsed_s + TELEMETRY_LOG_AUTOTUNE_PERIOD_S;
                if (s_enabled) {
                    int n = telemetry_format_autotune(&ast, line, sizeof(line));
                    if (n > 0) {
                        ESP_LOGI(TAG, "%s", line);
                    }
                }
            }
        } else {
            autotune_next_s = 0;
        }
        int autotune_code = autotune_event_code_for_transition(autotune_prev_state, ast.state);
        if (autotune_code >= 0) {
            event_log_severity_t sev = (autotune_code == EVENT_CODE_TUNE_ABORTED) ? EVENT_LOG_SEV_WARN
                                                                                   : EVENT_LOG_SEV_INFO;
            event_log_emit(LOG_STORE_KIND_AUTOTUNE, sev, EVENT_LOG_SRC_AUTOTUNE, (event_log_code_t)autotune_code,
                            ast.zone_index, (int32_t)ast.elapsed_s, NULL);
        }
        autotune_prev_state = ast.state;
    }
}

esp_err_t telemetry_log_start(void)
{
    if (s_task != NULL) {
        return ESP_OK; /* already running -- idempotent, same convention as the other *_start() calls */
    }
    /* PSRAM stack, matching autotune_engine.c/profile_executor.c's own
     * control tasks: this task never touches NVS or any flash-backed
     * storage (it only reads the two RAM-only status snapshots and calls
     * ESP_LOGI, which enqueues into uart_log_bridge.c's RAM queue), so the
     * "PSRAM stack must not be live while the flash cache is down" hazard
     * (uart_bridge_ext.c ~lines 91-156) does not apply to it the way it did
     * to profiles_task/control_task/autotune_task before those were routed
     * through bx_run_on_internal_stack() -- there is no flash call anywhere
     * in this task's call graph to trigger a cache-disable while this stack
     * is live. */
    /* 2026-09-07: the idle-board high-water mark (4096 B stack, 1048 B free,
     * 25.6% headroom [LOW]) was measured with s_enabled==false and no firing/
     * tune active -- telemetry_log_task's own `line[TELEMETRY_LOG_LINE_BUF]`
     * (320 B) is always resident in the frame regardless, but the deepest
     * call chain (s_enabled==true during an active firing with several
     * zones, an AUTOTUNE_ENGINE_DONE STEP-method line chaining ~15 float
     * conversions through snprintf, plus a same-tick event_log_emit() for a
     * state transition or ramp-lag edge) never ran on that board and was
     * never captured. Per CLAUDE.md the idle baseline is a floor, not a
     * measurement of the worst case, so this task's worst-of-three status
     * ([LOW] at 25.6%) is closed by growing the stack rather than trusting
     * the untested margin. This is a PSRAM stack (MALLOC_CAP_SPIRAM below,
     * same as autotune_engine/profile_executor's control tasks) so the
     * increase costs PSRAM, not the internal DRAM that is actually under
     * pressure (~78 kB free, HTTP socket resets seen near 11.9 kB) -- zero
     * DRAM cost for meaningfully more headroom on the untested deep path. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(telemetry_log_task, "telemetry_log", 6144, NULL,
                                                     3, &s_task, tskNO_AFFINITY,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(telemetry_log) failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. 6144 must match the xTaskCreatePinnedToCoreWithCaps() literal
     * above (grown from 4096 -- see the comment above that call). */
    stack_margin_register("telemetry_log", &s_task, 6144);
    return ESP_OK;
}
