#include "telemetry_log.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "autotune_engine.h"
#include "log_store_mount.h"
#include "profile_executor.h"
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

static void telemetry_log_task(void *arg)
{
    (void)arg;
    profile_exec_status_t fst;
    autotune_engine_status_t ast;
    char line[TELEMETRY_LOG_LINE_BUF];

    uint32_t firing_next_s = 0;
    uint32_t autotune_next_s = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_LOG_TICK_MS));

        /* Persistence to the on-flash log store (log_store_mount.c) runs
         * UNCONDITIONALLY, regardless of s_enabled -- s_enabled only gates
         * the live debug-UART feed (ESP_LOGI below), which this project's
         * own file banner documents as default-OFF and opt-in for a capture
         * session. The persistent record the 2026-09-01 audit asked for
         * ("logs are kept in external flash") is not something an operator
         * should have to remember to switch on before every firing -- it
         * uses the SAME formatted lines telemetry_format_firing()/
         * telemetry_format_autotune() already produce, just written to
         * "/logs" via log_store_write_firing()/log_store_write_autotune()
         * instead of (or as well as) the UART queue.
         *
         * Never blocks: profile_executor_get_status()/
         * autotune_engine_get_status() are documented snapshot copies that
         * never block on their owning task (profile_executor.h); the
         * ESP_LOGI() calls enqueue onto uart_log_bridge.c's queue with a
         * ZERO-tick xQueueSend (see this file's header comment); and
         * log_store_write_firing()/_autotune() block only on the internal-
         * stack flash worker (log_store_mount.c), never on this task's own
         * PSRAM stack touching flash directly. Nothing in this loop can
         * stall waiting on another task indefinitely. */
        profile_executor_get_status(&fst);
        if (fst.state == PROFILE_EXEC_RUNNING || fst.state == PROFILE_EXEC_PAUSED) {
            if (fst.total_elapsed_s >= firing_next_s) {
                firing_next_s = fst.total_elapsed_s + TELEMETRY_LOG_FIRING_PERIOD_S;
                int n = telemetry_format_firing(&fst, line, sizeof(line));
                if (n > 0) {
                    log_store_write_firing(line);
                    if (s_enabled) {
                        ESP_LOGI(TAG, "%s", line);
                    }
                }
            }
        } else {
            firing_next_s = 0; /* re-arm so the NEXT run logs its first tick immediately */
        }

        autotune_engine_get_status(&ast);
        bool at_active = (ast.state != AUTOTUNE_ENGINE_IDLE);
        if (at_active) {
            bool boundary = (ast.state == AUTOTUNE_ENGINE_DONE || ast.state == AUTOTUNE_ENGINE_ABORTED);
            if (boundary || ast.elapsed_s >= autotune_next_s) {
                autotune_next_s = ast.elapsed_s + TELEMETRY_LOG_AUTOTUNE_PERIOD_S;
                int n = telemetry_format_autotune(&ast, line, sizeof(line));
                if (n > 0) {
                    log_store_write_autotune(line);
                    if (s_enabled) {
                        ESP_LOGI(TAG, "%s", line);
                    }
                }
            }
        } else {
            autotune_next_s = 0;
        }
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
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(telemetry_log_task, "telemetry_log", 4096, NULL,
                                                     3, &s_task, tskNO_AFFINITY,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreatePinnedToCoreWithCaps(telemetry_log) failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
