#include "danger_mode.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kiln_io_owner.h"
#include "profile_executor_state.h"
#include "profile_executor.h"
#include "relay_authority.h" /* relay_authority_heat_run_active(): R1/R2 claim-first recheck */
#include "stack_margin.h"
#include "startup_faults.h"
#include "uart_task_ids.h" /* SAFETY_FLAG_RELAY/SAFETY_FLAG_ENABLED */

static const char *TAG = "danger_mode";
static TaskHandle_t s_task_handle; /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target */

#define DANGER_MODE_POLL_MS 1000u

typedef struct {
    SemaphoreHandle_t lock;
    bool     initialized;
    bool     window_open;
    uint32_t deadline_ms; /* now_ms() value the window closes at; meaningful only if window_open */
    SafetyLinkClass *safety; /* may be NULL -- see danger_mode_init()'s doc comment */
    /* This module's OWN outstanding request, set only by a successful
     * danger_mode_set_heat_enable_request(true) and cleared by an explicit
     * false, danger_mode_stop(), or timeout. NOT the same thing as
     * SAFETY_FLAG_ENABLED (safety_link_status_t::flags) -- that flag means
     * "SaftyFW's relay_owner state machine is ARMED / not tripped", which
     * on a healthy Pico is 1 whether or not anyone ever sent a REQUEST_
     * ENABLE. Bug found 2026-08-27 (Opus review): the diagnostics page's
     * "Firing mode" tile used to be driven straight from SAFETY_FLAG_
     * ENABLED, so it showed ON as soon as the Pico finished its boot grace
     * period -- with K4 correctly de-energized -- and every click then
     * computed "!already ON" = off, silently sending REQUEST_ENABLE(false)
     * forever with no way to actually request true. This field is the
     * fix: the tile now reflects what THIS module actually asked for. */
    bool     heat_requested;
} danger_mode_ctx_t;

static danger_mode_ctx_t s_dm;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool state_refuses_start(profile_exec_state_t state)
{
    return state == PROFILE_EXEC_RUNNING || state == PROFILE_EXEC_PAUSED;
}

bool danger_mode_request_start(void)
{
    /* danger_mode_task's own stack is 3072 B (the 2026-10-09 4096 B bump was reverted: measured
     * 800 B static lower bound, 780 B live used; DEV_STACK_ANALYSER_REVIEW_2026-10-09.md Q5), and this is also
     * reachable from the httpd task (diagnostics_http.c calls straight
     * into this function) -- a 1512-byte profile_exec_status_t stack local
     * would be a real bite out of either budget. state_refuses_start()
     * only needs RUNNING/PAUSED, so use profile_executor_get_active_id(),
     * the narrow accessor profile_executor.h recommends for exactly this,
     * instead of profile_executor_get_status(). */
    uint8_t active_id = 0;
    bool firing_active = profile_executor_get_active_id(&active_id);
    if (state_refuses_start(firing_active ? PROFILE_EXEC_RUNNING : PROFILE_EXEC_IDLE)) {
        ESP_LOGW(TAG, "danger mode refused -- a firing is in progress");
        return false;
    }
    if (!s_dm.initialized) {
        /* No expiry task exists to ever release/reboot this -- refuse
         * rather than send a REQUEST_ENABLE(true) that could only ever be
         * torn down by a full power cycle. Covers both "danger_mode_init()
         * was never called" and "it was called but xTaskCreate() failed",
         * same s_dm.initialized flag danger_mode_active()/remaining_ms()
         * already trust for the same reason. */
        ESP_LOGE(TAG, "danger mode refused -- not initialized (no expiry/reboot task)");
        return false;
    }
    if (!s_dm.lock || xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "danger mode refused -- could not take the internal lock");
        return false;
    }
    bool already_open = s_dm.window_open;
    bool was_heat_requested = s_dm.heat_requested;
    uint32_t was_deadline_ms = s_dm.deadline_ms;
    s_dm.window_open = true;
    s_dm.deadline_ms = now_ms() + DANGER_MODE_WINDOW_MS;
    s_dm.heat_requested = false;
    xSemaphoreGive(s_dm.lock);

    /* Claim-first, then recheck (LCD review R1/R2): the window is published ABOVE, and only now is
     * the heat claim (profile OR autotune, any state incl. PAUSED) read. profile_executor_run()/
     * autotune_begin_run_locked() publish their heat claim and THEN read danger_mode_blocks_start()
     * -- so at least one side sees the other: either this refuses, or the run's commit does. The
     * executor-state peek at the top of this function stays as the cheap early refusal. */
    bool heat_profile = false, heat_autotune = false;
    relay_authority_heat_run_active(&heat_profile, &heat_autotune);
    if (heat_profile || heat_autotune) {
        /* K7 review F1: the rollback must not be able to fail -- a failed 50 ms take left the
         * window open under a running firing (relay-route gates bypassed, link-loss drop
         * suppressed, the 5-minute expiry then cut the firing's heat). Holders of s_dm.lock are
         * short, so retry (with a log) until it is taken. s_dm.lock is non-NULL here (initialized
         * was checked and the first take succeeded). */
        while (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGE(TAG, "danger mode refusal rollback waiting for the internal lock");
        }
        {
            if (already_open) {
                s_dm.heat_requested = was_heat_requested; /* nothing changed */
                s_dm.deadline_ms = was_deadline_ms;       /* undo the extension too */
            } else {
                /* Deliberately NOT danger_mode_stop(): its enable=false release / all-relays-off
                 * would cut the run that just claimed heat. The window was open for microseconds
                 * and never had a request outstanding. */
                s_dm.window_open = false;
            }
            xSemaphoreGive(s_dm.lock);
        }
        ESP_LOGW(TAG, "danger mode refused -- a %s run holds the heat claim",
                 heat_autotune ? "autotune" : "firing");
        return false;
    }
    if (already_open && was_heat_requested) {
        /* F6-1: re-entry cleared heat_requested above; send the matching release so the Pico's
         * request and the tile agree (display and wire never disagree). */
        esp_err_t rel_err = safety_link_request_enable(s_dm.safety, false);
        if (rel_err != ESP_OK) {
            ESP_LOGE(TAG, "danger mode re-entered -- heat-enable release request failed (%s); Pico request may still be set", esp_err_to_name(rel_err));
        } else {
            ESP_LOGW(TAG, "danger mode re-entered -- heat-enable request released to match the cleared flag");
        }
    }

    /* Does NOT request heat-enable on its own any more (owner request
     * 2026-08-27) -- entering this section only unlocks the ESP's own
     * four-relay bypass. K4/heat-enable is now a separate, explicit
     * operator action -- see danger_mode_set_heat_enable_request() below. */
    if (!already_open) {
        ESP_LOGW(TAG, "DANGER MODE ENTERED: this board's own gate on its four relays is now "
                      "bypassed for %lu ms, extended on every command, until an operator stops it "
                      "or it sits idle long enough to auto-exit. K4/heat-enable is NOT requested by "
                      "entering this section -- see danger_mode_set_heat_enable_request().",
                 (unsigned long)DANGER_MODE_WINDOW_MS);
    }
    return true;
}

bool danger_mode_set_heat_enable_request(bool enable)
{
    /* Only meaningful, and only allowed, while the window is open -- same
     * "this section's own actions" gate danger_relay_post_handler()
     * (diagnostics_http.c) already enforces for the four ESP-owned relays;
     * K4 gets the identical treatment now that it is its own explicit
     * action rather than an automatic side effect of entering. */
    if (!danger_mode_active()) {
        return false;
    }
    /* Same request a real firing sends -- SaftyFW's own guards decide
     * whether K4 actually closes; see danger_mode.h's top comment. UNLIKE
     * the old code, the result IS checked now: safety_link_request_enable()
     * returns ESP_ERR_INVALID_STATE for enable=true when the link is down
     * and sends nothing on the wire (safety_link.c) -- reporting success to
     * the UI in that case would make heat_requested (and the tile it
     * drives) claim a request that was never actually sent. A release
     * (enable=false) still always "succeeds" from this module's point of
     * view -- see danger_mode_stop()'s own unconditional release, same
     * reasoning. */
    esp_err_t err = safety_link_request_enable(s_dm.safety, enable);
    if (enable && err != ESP_OK) {
        ESP_LOGW(TAG, "danger mode: heat-enable request NOT sent: %s", esp_err_to_name(err));
        return false;
    }
    if (!s_dm.lock || xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        /* The wire request above already went out (or, for enable=false,
         * there was nothing to send) -- but heat_requested cannot be
         * updated to match, so the tile would show the OLD state while
         * reality is the new one. Opus review 2026-08-27: the original code
         * still returned true here, silently reintroducing this fix's own
         * failure mode (display disagreeing with what was actually sent) on
         * the rare lock-contention path instead of the "wrong flag read"
         * path. Reporting failure here is honest either way: for
         * enable=true a caller retries as a fresh explicit action; for
         * enable=false the window's own timeout/stop path still forces a
         * release+heat_requested=false unconditionally, so nothing is
         * permanently stuck. */
        ESP_LOGE(TAG, "danger mode: heat-enable request sent but internal lock timed out -- "
                      "heat_requested NOT updated, retry");
        return false;
    }
    s_dm.heat_requested = enable;
    xSemaphoreGive(s_dm.lock);
    ESP_LOGW(TAG, "danger mode: operator %s heat-enable request", enable ? "sent" : "released");
    danger_mode_touch();
    return true;
}

bool danger_mode_get_heat_requested(void)
{
    if (!s_dm.initialized || !s_dm.lock) {
        return false;
    }
    bool requested = false;
    if (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        requested = s_dm.window_open && s_dm.heat_requested;
        xSemaphoreGive(s_dm.lock);
    }
    return requested;
}

bool danger_mode_touch(void)
{
    if (!s_dm.lock || xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    bool touched = false;
    if (s_dm.window_open) {
        s_dm.deadline_ms = now_ms() + DANGER_MODE_WINDOW_MS;
        touched = true;
    }
    xSemaphoreGive(s_dm.lock);
    return touched;
}

bool danger_mode_active(void)
{
    if (!s_dm.initialized || !s_dm.lock) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_dm.window_open) {
            /* Wraparound-safe, same idiom as boot_button.c/ota_auth.c: a
             * signed remaining-time compare would misbehave across the
             * xTaskGetTickCount() wrap; testing "deadline has not yet
             * arrived" via unsigned subtraction does not.
             *
             * Deliberately does NOT clear window_open when this reads false
             * past the deadline (unlike boot_button_ota_bypass_active()'s
             * own lazy-close) -- gating callers like kiln_io_owner.c's
             * relay_on_blocked() still see the bypass end exactly on time
             * either way, but ONLY danger_mode_task() may ever flip
             * window_open to false, and only while also performing the
             * release+reboot that must go with it. This function and
             * danger_mode_remaining_ms() below are read constantly (every
             * HTTP status poll, every relay-authority check); if either of
             * them raced the once-a-second task to clear the flag first,
             * the task would see window_open already false, skip its
             * expired branch entirely, and neither SAFETY_CMD_REQUEST_
             * ENABLE(false) nor the auto-reboot would ever run -- exactly
             * the silent-stuck-enabled bug a lazy self-close here would
             * cause. */
            active = (int32_t)(s_dm.deadline_ms - now_ms()) > 0;
        }
        xSemaphoreGive(s_dm.lock);
    } else {
        ESP_LOGW(TAG, "danger_mode_active: internal lock timeout, reporting inactive");
    }
    return active;
}

bool danger_mode_blocks_start(void)
{
    if (!s_dm.initialized || !s_dm.lock) {
        return false; /* no window can exist */
    }
    bool blocks = true; /* unknown (lock timeout) reads as blocked */
    if (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        blocks = s_dm.window_open && (int32_t)(s_dm.deadline_ms - now_ms()) > 0;
        xSemaphoreGive(s_dm.lock);
    } else {
        ESP_LOGW(TAG, "danger_mode_blocks_start: internal lock timeout, refusing the start");
    }
    return blocks;
}

uint32_t danger_mode_remaining_ms(void)
{
    if (!s_dm.initialized || !s_dm.lock) {
        return 0;
    }
    uint32_t remaining = 0;
    if (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_dm.window_open) {
            /* Same "never self-close here" reasoning as danger_mode_active()
             * above -- only danger_mode_task() may clear window_open. */
            int32_t left = (int32_t)(s_dm.deadline_ms - now_ms());
            if (left > 0) {
                remaining = (uint32_t)left;
            }
        }
        xSemaphoreGive(s_dm.lock);
    }
    return remaining;
}

bool danger_mode_get_relay_status(bool *out_relay_energized, bool *out_heating_enabled)
{
    if (out_relay_energized) {
        *out_relay_energized = false;
    }
    if (out_heating_enabled) {
        *out_heating_enabled = false;
    }
    if (!s_dm.safety) {
        return false;
    }
    safety_link_status_t st;
    if (safety_link_get_status(s_dm.safety, &st) != ESP_OK || !st.link_up) {
        return false;
    }
    if (out_relay_energized) {
        *out_relay_energized = (st.flags & SAFETY_FLAG_RELAY) != 0u;
    }
    if (out_heating_enabled) {
        *out_heating_enabled = (st.flags & SAFETY_FLAG_ENABLED) != 0u;
    }
    return true;
}

void danger_mode_stop(const char *source)
{
    const char *src = source ? source : "unknown";
    if (!s_dm.lock) {
        return;
    }
    bool was_open = false;
    if (xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        was_open = s_dm.window_open;
        s_dm.window_open = false;
        s_dm.heat_requested = false;
        xSemaphoreGive(s_dm.lock);
    }
    if (was_open) {
        /* Restoring the gate only refuses the NEXT relay-ON; it does nothing
         * about a relay danger mode already closed. Confirmed on the bench:
         * energize a relay from the diagnostics page, press Stop, and the
         * coil stays closed indefinitely -- under a gate that would now
         * refuse to close it, so nothing on the normal path ever opens it
         * again either. Exiting the window has to leave the board in the
         * state it would be in had the window never opened. */
        esp_err_t relay_err = kiln_io_owner_command_all_relays_off();
        esp_err_t enable_err = safety_link_request_enable(s_dm.safety, false);
        if (relay_err != ESP_OK || enable_err != ESP_OK) {
            /* Both calls used to be cast to (void) and this message printed
             * unconditionally -- so an owner-queue timeout (ESP_ERR_TIMEOUT,
             * the busy/backed-up case, not hypothetical) looked identical in
             * the log to relays actually dropping. Surface the disagreement;
             * the attempt itself is unchanged. */
            ESP_LOGE(TAG, "danger mode stopped (%s) -- relay/enable release FAILED "
                          "(relays=%s enable=%s); do not assume the coil is open",
                     src, esp_err_to_name(relay_err), esp_err_to_name(enable_err));
        } else {
            ESP_LOGW(TAG, "danger mode stopped (%s) -- relays dropped, relay gate restored, "
                          "heat-enable released, no reboot",
                     src);
        }
    }
}

/* The one task that acts on expiry-by-timeout: reading the flag from many
 * callers (danger_mode_active()) must never itself perform the release, or a
 * burst of concurrent HTTP requests right at the deadline could each try to
 * send it. Polls once a second -- this is a 5-minute window, not a
 * hard-real-time deadline, so coarse polling costs nothing an operator would
 * notice. */
static void danger_mode_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DANGER_MODE_POLL_MS));

        bool expired = false;
        if (s_dm.lock && xSemaphoreTake(s_dm.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_dm.window_open && (int32_t)(s_dm.deadline_ms - now_ms()) <= 0) {
                s_dm.window_open = false;
                s_dm.heat_requested = false;
                expired = true;
            }
            xSemaphoreGive(s_dm.lock);
        }

        if (expired) {
            /* Same graceful release danger_mode_stop() performs for an
             * operator-requested exit -- see danger_mode.h's top comment:
             * timeout exits exactly as though the firing this request stood
             * in for had ended, no reboot. */
            /* Same reason as danger_mode_stop()'s -- and more pressing here,
             * since a timeout means nobody is watching the page. */
            esp_err_t relay_err = kiln_io_owner_command_all_relays_off();
            esp_err_t enable_err = safety_link_request_enable(s_dm.safety, false);
            if (relay_err != ESP_OK || enable_err != ESP_OK) {
                ESP_LOGE(TAG, "danger mode timed out (%lu ms idle) -- relay/enable release "
                              "FAILED (relays=%s enable=%s); do not assume the coil is open",
                         (unsigned long)DANGER_MODE_WINDOW_MS,
                         esp_err_to_name(relay_err), esp_err_to_name(enable_err));
            } else {
                ESP_LOGW(TAG, "danger mode timed out (%lu ms idle) -- relays dropped, heat-enable "
                              "released, relay gate restored, no reboot",
                         (unsigned long)DANGER_MODE_WINDOW_MS);
            }
        }
    }
}

void danger_mode_init(SafetyLinkClass *safety)
{
    if (s_dm.initialized) {
        return; /* safe to call more than once, same convention as boot_button_start() */
    }
    s_dm.lock = xSemaphoreCreateMutex();
    if (!s_dm.lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- danger mode will not be available this boot");
        return;
    }
    s_dm.window_open = false;
    s_dm.safety = safety;
    s_dm.initialized = true;

    /* Plain xTaskCreate, internal-RAM stack -- this task can call
     * esp_restart(), and boot_button.c/ota_http.c's own task-creation
     * comments already establish why a PSRAM-stack task must never be the
     * one holding a stack frame across a reboot path in this codebase. */
    if (xTaskCreate(danger_mode_task, "danger_mode", 3072, NULL, tskIDLE_PRIORITY + 1, &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(danger_mode_task) failed -- danger mode will not be available "
                      "this boot");
        startup_fault_note(STARTUP_FAULT_DANGER_MODE);
        s_dm.initialized = false;
        return;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above now
     * returns. 3072 must match the xTaskCreate() literal above. */
    stack_margin_register("danger_mode", &s_task_handle, 3072);
}
