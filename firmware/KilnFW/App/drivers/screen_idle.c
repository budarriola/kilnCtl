#include "screen_idle.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"
#include "settings.h"
#include "stack_margin.h"

// 2026-09-04 owner request (docs/UI_PLAN.md "Display power"): the pure
// decision core (display_power_policy.h) and its real inputs -- the
// persisted brightness/timeout/keep-on/display-on-error settings
// (display_power_cfg.h, already landed, not modified by this pass), the
// REAL firing-active producer (profile_executor_get_status() -- the same
// accessor boot_button.c/danger_mode.c/gpio_probe.c already use for exactly
// this "is a firing live" question), and the REAL error/fault producer
// (dashboard_get_status()'s diag_ever_received/diag_state/diag_age_ms --
// the identical fields+gate ui_page_home.c's own safety-trip strip already
// keys off, so this module raises the display for precisely the condition
// the LCD already paints red, not a second, possibly-different notion of
// "error"). dashboard_http.c/profile_executor.c are READ ONLY from here
// (their public accessors) -- neither is edited by this pass.
#include "display_power_policy.h"
#include "display_power_cfg.h"
#include "profile_executor.h"
#include "dashboard_http.h"
#include "autotune_engine.h"

static const char *TAG = "screen_idle";

/* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target. Only one
 * screen_idle_t instance exists (main.c's static screen_idle), so a single
 * module-static handle is enough -- matches kiln_io_owner.c/thermo_owner.c's
 * s_task_handle idiom for owners whose public struct has no handle field. */
static TaskHandle_t s_task_handle;

/* How often screen_idle_task polls the touch controller (when one is up).
 * NS2009_read does three back-to-back I2C conversions worst case (Z1, then
 * X and Y only if pressed); 50ms is comfortably above that and fast enough
 * that a touch feels immediate against a 60s timeout. */
#define SCREEN_IDLE_POLL_MS 50u

/* How long a caller waits for the lock. Only ever held for a few variable
 * reads/writes, never across I2C or SPI traffic -- generous on purpose. */
#define SCREEN_IDLE_LOCK_TIMEOUT_MS 1000u

static bool screen_idle_lock(screen_idle_t *idle)
{
    return xSemaphoreTake(idle->lock, pdMS_TO_TICKS(SCREEN_IDLE_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void screen_idle_unlock(screen_idle_t *idle)
{
    xSemaphoreGive(idle->lock);
}

static uint32_t screen_idle_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* THE single call site for display_power_policy_step() -- both the poll
 * task's idle-timeout tick and a touch edge (screen_idle_touch_swallow())
 * fund it through here so the edge-tracked inputs (error_prev_active,
 * touch_held/touch_held_swallow) are never computed two different ways.
 * MUST be called with idle->lock already held; never takes/releases it
 * itself (same "caller's discipline" convention profile_executor_wd_
 * decide()'s doc comment describes for its own pure-step neighbor). Reads
 * profile_executor_get_status()/dashboard_get_status() while holding this
 * module's own lock -- safe: neither of those modules ever calls back into
 * screen_idle, so there is no lock-order cycle, only a leaf read.
 *
 * Returns the policy's swallow_touch (meaningful only when touch_event is
 * true); always updates idle->policy_state/screen_on/last_activity_tick/
 * error_prev_active as a side effect. */
static bool screen_idle_run_policy_locked(screen_idle_t *idle, uint32_t now_ms, bool touch_event)
{
    // 2026-09-04 bench crash (boot_guard.h RECOVERY MODE, bricked the board
    // for 495+ consecutive boots): recovery mode starts THIS task
    // unconditionally (main_boot_early.c, well before main_control_
    // bringup.c's recovery-mode skip of profile_executor_start()/
    // autotune_engine_start()) and every poll tick blocked here forever --
    // see screen_idle.h's idle->recovery_mode field comment for the exact
    // hardware-confirmed backtrace and why this caller-side gate exists
    // alongside (not instead of) those subsystems' own not-started guards.
    // Recovery mode genuinely never runs anything that could make either
    // condition true, so "false" is the honest answer, not a stand-in.
    if (idle->recovery_mode) {
        bool firing_active = false;
        bool error_active = false;
        bool error_entered_this_tick = false;
        idle->error_prev_active = false;

        display_power_input_t in = {
            .now_ms = now_ms,
            .last_touch_ms = (uint32_t)(idle->last_activity_tick * portTICK_PERIOD_MS),
            .timeout_setting = display_power_cfg_timeout_setting(),
            .firing_active = firing_active,
            .keep_on_while_firing = display_power_cfg_keep_on_while_firing(),
            .error_active = error_active,
            .error_entered_this_tick = error_entered_this_tick,
            .display_on_error = display_power_cfg_display_on_error(),
            .current_state = idle->policy_state,
            .touch_event = touch_event,
        };
        display_power_result_t out = display_power_policy_step(&in);

        if (out.state != idle->policy_state) {
            ESP_LOGI(TAG, "display power: state %d -> %d (recovery mode, touch=%d)",
                     (int)idle->policy_state, (int)out.state, (int)touch_event);
        }
        idle->policy_state = out.state;
        idle->screen_on = (out.state != DISPLAY_POWER_OFF);
        if (touch_event) {
            idle->last_activity_tick = xTaskGetTickCount();
        }
        return out.swallow_touch;
    }

    profile_exec_status_t pst;
    profile_executor_get_status(&pst);
    // 2026-09-04 opus review: profile_executor is NOT the only producer that
    // means "the kiln is heating and the owner needs to see the screen".
    // autotune_engine drives relays on its own for hours with the executor
    // sitting at PROFILE_EXEC_IDLE (autotune_engine.h: "Both methods hold the
    // zone's relay authority for exactly as long as this is true") -- reading
    // only the executor is precisely this codebase's "consumer reading a
    // different producer than the one that actually gets written" bug class
    // (project_autotune_feeds_fake_setpoint: "guard rules reading setpoint_c
    // break autotune while the executor stays green; check BOTH producers").
    // Without this OR, "keep display on while firing" blanks the panel in the
    // middle of an autotune run.
    bool firing_active = (pst.state == PROFILE_EXEC_RUNNING || pst.state == PROFILE_EXEC_PAUSED) ||
                         autotune_engine_is_active();

    dashboard_status_t ds;
    dashboard_get_status(&ds);
    // Identical gate to ui_page_home.c's own s_trip_strip condition (that
    // file's comment: "a STALE diag_state == TRIPPED is a silent link, not
    // a live trip") -- this module must raise the display for exactly the
    // condition the LCD already paints red, not a differently-gated
    // "error" of its own invention.
    bool safety_tripped = ds.diag_ever_received &&
                          ds.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED &&
                          ds.diag_age_ms < SAFETY_LINK_STALE_MS;
    // 2026-09-04 opus review: the safety-link trip is the RP2040's OWN trip.
    // The ESP's own global thermal-guard abort (PROFILE_EXEC_FAULTED --
    // profile_executor.h: "a GLOBAL thermal guard tripped (or every active
    // zone individually)", carrying fault_reason/fault_guard) never touches
    // diag_state, so keying "error" on the safety link alone silently missed
    // an entire class of error the owner would absolutely expect to raise the
    // display: their firing just aborted. Both producers, not one.
    bool error_active = safety_tripped || pst.state == PROFILE_EXEC_FAULTED;
    bool error_entered_this_tick = error_active && !idle->error_prev_active;
    idle->error_prev_active = error_active;

    display_power_input_t in = {
        .now_ms = now_ms,
        .last_touch_ms = (uint32_t)(idle->last_activity_tick * portTICK_PERIOD_MS),
        .timeout_setting = display_power_cfg_timeout_setting(),
        .firing_active = firing_active,
        .keep_on_while_firing = display_power_cfg_keep_on_while_firing(),
        .error_active = error_active,
        .error_entered_this_tick = error_entered_this_tick,
        .display_on_error = display_power_cfg_display_on_error(),
        .current_state = idle->policy_state,
        .touch_event = touch_event,
    };
    display_power_result_t out = display_power_policy_step(&in);

    if (out.state != idle->policy_state) {
        ESP_LOGI(TAG, "display power: state %d -> %d (firing=%d error=%d touch=%d)",
                 (int)idle->policy_state, (int)out.state, (int)firing_active, (int)error_active,
                 (int)touch_event);
    }
    idle->policy_state = out.state;
    idle->screen_on = (out.state != DISPLAY_POWER_OFF);
    // Header contract: "the caller must still update ITS OWN last-activity
    // timestamp to now_ms" whenever a touch is swallowed OR passed through
    // -- and every poll tick this function is called from is itself real
    // activity-adjacent bookkeeping (mirrors the old screen_idle_mark_
    // active()'s unconditional stamp). Not gated on touch_event: the poll
    // tick must NOT keep stamping activity every 50ms (that would defeat
    // the idle timeout entirely) -- see the caller below, which only lets
    // this land on a genuine touch edge.
    if (touch_event) {
        idle->last_activity_tick = xTaskGetTickCount();
    }
    return out.swallow_touch;
}

static void screen_idle_task(void *arg)
{
    screen_idle_t *idle = (screen_idle_t *)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCREEN_IDLE_POLL_MS));

        if (idle->touch) {
            bool pressed = false;
            uint16_t x = 0, y = 0;
            esp_err_t err = NS2009_read(idle->touch, &pressed, &x, &y, NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "NS2009_read failed: %s", esp_err_to_name(err));
            } else if (pressed) {
                bool swallow_unused;
                screen_idle_touch_swallow(idle, x, y, true, &swallow_unused);
                // This task's own direct NS2009 poll only runs when
                // lvgl_port.c was started WITHOUT a touch_dev_t (idle->touch
                // non-NULL implies lvgl_port never took over touch reads --
                // see screen_idle.h's top comment and lvgl_port.h's
                // touch_dev_t contract). In that configuration nothing else
                // could have delivered this press to LVGL for swallowing to
                // matter against, so the return value genuinely has no
                // caller here -- this branch exists purely so the idle
                // timer / display-power state machine still runs when no
                // on-device UI is driving touch at all.
            }
        }

        // Idle-timeout / keep-on-while-firing / display-on-error poll tick
        // -- touch_event=false, so this can never itself produce a swallow
        // decision or re-stamp last_activity_tick (see screen_idle_run_
        // policy_locked()'s comment). Runs every SCREEN_IDLE_POLL_MS
        // regardless of current screen_on, same as the pure policy step
        // itself needs to see (an OFF->ERROR_HOLD transition, for example,
        // must be evaluated while already OFF).
        if (!screen_idle_lock(idle)) continue;
        screen_idle_run_policy_locked(idle, screen_idle_now_ms(), false);
        screen_idle_unlock(idle);
    }
}

esp_err_t screen_idle_init(screen_idle_t *idle, ILI9488Class *display, NS2009Class *touch,
                           bool recovery_mode)
{
    if (!idle || !display) return ESP_ERR_INVALID_ARG;

    memset(idle, 0, sizeof(*idle));
    idle->display = display;
    idle->touch = touch;
    idle->recovery_mode = recovery_mode;
    idle->last_activity_tick = xTaskGetTickCount();
    idle->screen_on = true; /* the display driver leaves the panel lit after bring-up */
    idle->policy_state = DISPLAY_POWER_ON; /* display_power_policy_step()'s documented first-call value */

    idle->lock = xSemaphoreCreateMutex();
    if (!idle->lock) return ESP_ERR_NO_MEM;

    idle->ready = true;
    return ESP_OK;
}

esp_err_t screen_idle_start(screen_idle_t *idle)
{
    if (!idle || !idle->ready) return ESP_ERR_INVALID_STATE;

    /* 2026-09-04 bench crash: this was 3072 until today, sized for the old
     * "read the touch controller, run the pure policy step" job. 7fc17cc/
     * 192eb7d (this same day) put a MUCH deeper call chain behind every
     * poll tick's non-recovery-mode path -- profile_executor_get_status(),
     * dashboard_get_status() (which itself declares a local MAX31856Reading
     * readings[MAX31856_CHANNEL_COUNT] and, worse, calls heap_caps_get_
     * largest_free_block()/heap_caps_get_info(), a full internal-heap
     * TLSF-pool walk), and autotune_engine_is_active() -- plus screen_idle_
     * run_policy_locked()'s own locals (profile_exec_status_t pst,
     * dashboard_status_t ds, both nontrivial structs). Every OTHER caller
     * of dashboard_get_status() in this codebase runs on a task sized for
     * it: lvgl_port.c's own task asks for 8192 (see that file's xTaskCreate
     * PinnedToCoreWithCaps() comment). screen_idle_task's unchanged 3072
     * was never re-measured against its new job -- it did not survive
     * contact with hardware: a normal (non-recovery) boot corrupted the
     * internal DRAM heap (esp_core_dump_elf: "Block corruption detected in
     * the heap") and then panicked (LoadProhibited) inside dashboard_get_
     * status()'s heap_caps_get_largest_free_block() call, on a heap block
     * sitting immediately past where this task's own (heap-allocated)
     * stack ends -- addr2line on the COM3 console backtrace named screen_
     * idle_task -> screen_idle_run_policy_locked -> dashboard_get_status ->
     * heap_caps_get_largest_free_block -> tlsf_walk_pool exactly. Recovery
     * mode's early-return path (this file's idle->recovery_mode branch)
     * never reaches that deep call chain, which is why recovery mode alone
     * did not reproduce this second bug even though it shares the same
     * task. 6144 is a deliberately generous doubling, not a measured
     * number -- stack_margin.h's own header comment (TODO.md section 13)
     * warns this repo has already shipped a stack overflow from a size
     * "chosen from a comment rather than a measurement": get_stack_margin()
     * (kilnctrl MCP) must be checked against real headroom on hardware
     * after this change, same as any other task's stack size here. */
    BaseType_t created =
        xTaskCreatePinnedToCore(screen_idle_task, "screen_idle", 6144, idle, 3, &s_task_handle, tskNO_AFFINITY);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. 6144 must match the xTaskCreatePinnedToCore() literal above. */
    stack_margin_register("screen_idle", &s_task_handle, 6144);
    return ESP_OK;
}

esp_err_t screen_idle_inject_touch(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed)
{
    // Thin wrapper over screen_idle_touch_swallow() that discards the
    // swallow verdict -- this function's callers (uart_bridge_touch.c's
    // TOUCH_CMD_INJECT handler) only ever wanted the wake/idle-timer side
    // effect; the actual LVGL delivery for that same wire event is a
    // SEPARATE call (lvgl_port_inject_touch(), read back by touch_read_cb(),
    // which is the call site that owns the real swallow decision -- see
    // that function's own screen_idle_touch_swallow() call). Both paths
    // share the one edge-tracked state in `idle` (touch_held et al.), so
    // whichever of the two calls observes a press transition first computes
    // the edge; the other lands as a harmless repeat. See screen_idle.h's
    // touch_held field comment.
    bool swallow_unused;
    return screen_idle_touch_swallow(idle, x, y, pressed, &swallow_unused);
}

esp_err_t screen_idle_touch_swallow(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed,
                                    bool *out_swallow)
{
    if (!out_swallow) return ESP_ERR_INVALID_ARG;
    *out_swallow = false;
    if (!idle || !idle->ready) return ESP_ERR_INVALID_STATE;
    (void)x;
    (void)y; /* not used for the idle/wake/swallow decision -- only *that* a touch happened, and its press/release edge */

    if (!screen_idle_lock(idle)) return ESP_ERR_TIMEOUT;

    if (!pressed) {
        // A release: clears the held-press edge tracker so the NEXT press
        // is evaluated as a fresh edge. Never itself swallowed (there is
        // nothing new to decide) -- if the press that is now releasing was
        // swallowed, the caller was already reporting RELEASED to LVGL for
        // every poll of it (see screen_idle.h's touch_held comment), so
        // this changes nothing observable, only resets bookkeeping.
        idle->touch_held = false;
        screen_idle_unlock(idle);
        return ESP_OK;
    }

    uint32_t now_ms = screen_idle_now_ms();
    bool was_wake = !idle->screen_on;

    if (!idle->touch_held) {
        // Press EDGE: exactly one display_power_policy_step() call with
        // touch_event=true per the header's calling contract.
        idle->touch_held = true;
        idle->touch_held_swallow = screen_idle_run_policy_locked(idle, now_ms, true);
    }
    // Repeat within the same held press: return the edge's cached verdict,
    // do NOT re-run the policy (would violate "exactly one call per edge").

    *out_swallow = idle->touch_held_swallow;
    screen_idle_unlock(idle);

    if (was_wake) ESP_LOGI(TAG, "screen woken");
    return ESP_OK;
}

esp_err_t screen_idle_get_state(const screen_idle_t *idle, bool *out_screen_on,
                                uint32_t *out_idle_ms)
{
    if (!idle || !idle->ready || !out_screen_on || !out_idle_ms) return ESP_ERR_INVALID_ARG;

    /* const-correctness note: the lock itself is not modified through this
     * pointer's constness in any way the compiler would reject, but taking
     * it requires a non-const handle -- cast away const on the semaphore
     * only, mirroring how ILI9488_get_dimensions-style read-only queries are
     * written elsewhere in this codebase. */
    screen_idle_t *mutable_idle = (screen_idle_t *)idle;
    if (!screen_idle_lock(mutable_idle)) return ESP_ERR_TIMEOUT;
    TickType_t last_activity_tick = idle->last_activity_tick;
    *out_screen_on = idle->screen_on;
    screen_idle_unlock(mutable_idle);

    TickType_t elapsed_ticks = xTaskGetTickCount() - last_activity_tick;
    uint64_t elapsed_ms = (uint64_t)elapsed_ticks * portTICK_PERIOD_MS;
    *out_idle_ms = (elapsed_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed_ms;
    return ESP_OK;
}
