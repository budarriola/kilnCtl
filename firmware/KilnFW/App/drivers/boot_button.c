#include "boot_button.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "profile_executor.h"

static const char *TAG = "boot_button";

// GPIO0 -- the ESP32-S3 BOOT strap pin. Confirmed free of any other claim in
// this firmware before wiring it here: grepped this whole tree for
// GPIO_NUM_0/gpio_num_t)0/"GPIO0" and found nothing else in App/ touches it
// (docs/HARDWARE.md's one "GPIO0" mention is the SEPARATE RP2040 safety
// processor's own GPIO0, an unrelated chip/pin; the two LVGL "EVE_GPIO0"
// hits are an unrelated vendor macro name in components/lvgl's FT81x driver,
// never instantiated by this board's ILI9488 display path). Same pin the
// bootloader itself samples at reset -- see boot_button.h's ROM-download-
// mode caveat for why that matters here.
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#define BOOT_BUTTON_POLL_MS 100u

// --- Pure state machine -----------------------------------------------------

boot_button_event_t boot_button_step(boot_button_press_state_t *st, bool pressed, uint32_t now_ms)
{
    if (!pressed) {
        st->pressed_last = false;
        st->fired_this_press = false;
        return BOOT_BUTTON_EVENT_NONE;
    }

    if (!st->pressed_last) {
        // Rising edge: a new press begins now.
        st->pressed_last = true;
        st->press_started_ms = now_ms;
        st->fired_this_press = false;
        return BOOT_BUTTON_EVENT_NONE;
    }

    if (st->fired_this_press) {
        // Already fired for this continuous press -- a held button must not
        // re-fire on every subsequent 100ms tick.
        return BOOT_BUTTON_EVENT_NONE;
    }

    // Wraparound-safe elapsed time, same subtraction convention as
    // ota_auth.c's nonce-expiry check (see this header's doc comment on
    // boot_button_step()).
    uint32_t held_ms = (uint32_t)(now_ms - st->press_started_ms);
    if (held_ms >= BOOT_BUTTON_HOLD_MS) {
        st->fired_this_press = true;
        return BOOT_BUTTON_EVENT_OPEN_REQUESTED;
    }

    return BOOT_BUTTON_EVENT_NONE;
}

// --- On-target state ---------------------------------------------------------

typedef struct {
    SemaphoreHandle_t lock;
    bool     initialized;
    bool     window_open;
    uint32_t window_opened_ms;   /* now_ms() at the tick the window opened; meaningful only if window_open */
    boot_button_press_state_t press;
} boot_button_ctx_t;

static boot_button_ctx_t s_bb;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

// True if `state` is one this hatch refuses to open the window during --
// see boot_button.h's "WHY A FIRING BLOCKS IT" for the full reasoning.
static bool state_refuses_bypass(profile_exec_state_t state)
{
    return state == PROFILE_EXEC_RUNNING || state == PROFILE_EXEC_PAUSED;
}

static void open_window_locked(uint32_t at_ms)
{
    s_bb.window_open = true;
    s_bb.window_opened_ms = at_ms;
    ESP_LOGE(TAG, "**********************************************************");
    ESP_LOGE(TAG, "BOOT-BUTTON RECOVERY: OTA authentication is now BYPASSED for %lu ms "
                  "(until an operator flashes, or the window auto-closes).",
             (unsigned long)BOOT_BUTTON_WINDOW_MS);
    ESP_LOGE(TAG, "Any device on this network can now push firmware with NO password check.");
    ESP_LOGE(TAG, "**********************************************************");
}

// Called from the poll task on BOOT_BUTTON_EVENT_OPEN_REQUESTED. Reads
// profile_executor_get_status() itself (not passed in) -- this is the one
// place the pure step() function's decision is turned into the actual
// refuse/open call, matching ota_http.c's own pattern of keeping the I/O
// (here, the executor-status read and the lock) at the call site and the
// pure logic (state_refuses_bypass()) separate and trivially testable.
static void handle_open_requested(void)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);

    if (state_refuses_bypass(st.state)) {
        ESP_LOGE(TAG, "BOOT-BUTTON RECOVERY: long-press detected but REFUSED -- a firing is in "
                      "progress (profile_exec state=%d). Halt or wait for the firing to finish, "
                      "then press and hold the BOOT button again while the board is running.",
                 (int)st.state);
        return;
    }

    uint32_t t = now_ms();
    if (xSemaphoreTake(s_bb.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "BOOT-BUTTON RECOVERY: long-press detected but could not take the internal "
                      "lock -- refusing rather than risk a torn window state");
        return;
    }
    open_window_locked(t);
    xSemaphoreGive(s_bb.lock);
}

static void boot_button_task(void *arg)
{
    (void)arg;
    for (;;) {
        // ACTIVE LOW: pulled up internally, pulled to ground by the button.
        bool pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
        boot_button_event_t ev = boot_button_step(&s_bb.press, pressed, now_ms());
        if (ev == BOOT_BUTTON_EVENT_OPEN_REQUESTED) {
            handle_open_requested();
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}

void boot_button_start(void)
{
    if (s_bb.initialized) {
        return; /* safe to call more than once, same convention as boot_guard_init() */
    }

    s_bb.lock = xSemaphoreCreateMutex();
    if (!s_bb.lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- boot-button recovery hatch will not be "
                      "available this boot");
        return;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE, /* polled, not ISR-driven -- see boot_button.h */
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO0) failed: %s -- boot-button recovery hatch will not be "
                      "available this boot",
                 esp_err_to_name(err));
        return;
    }

    memset(&s_bb.press, 0, sizeof(s_bb.press));
    s_bb.window_open = false;
    s_bb.initialized = true;

    // Plain xTaskCreate -- INTERNAL-RAM stack, deliberately. See this
    // module's header comment ("TASK / STACK PLACEMENT") for why: this
    // project treats a PSRAM-backed task stack running with the flash cache
    // disabled as a standing crash risk, matching ota_http.c's
    // ota_recovery_exit_reboot_task() and profile_executor.c's own task-
    // creation precedent. 3072 words is generous for a loop this small
    // (one gpio_get_level(), one pure function call, an occasional
    // profile_executor_get_status() snapshot, some ESP_LOGx calls).
    if (xTaskCreate(boot_button_task, "boot_button", 3072, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(boot_button_task) failed -- boot-button recovery hatch will not "
                      "be available this boot");
        s_bb.initialized = false;
    }
}

bool boot_button_ota_bypass_active(void)
{
    if (!s_bb.initialized || !s_bb.lock) {
        return false;
    }
    bool active = false;
    if (xSemaphoreTake(s_bb.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_bb.window_open) {
            // Evaluated fresh against the current clock every call -- see
            // boot_button.h's doc comment on why this must never trust a
            // cached "still open" flag past the real deadline.
            uint32_t elapsed = (uint32_t)(now_ms() - s_bb.window_opened_ms);
            active = elapsed < BOOT_BUTTON_WINDOW_MS;
            if (!active) {
                // Lazily close on read -- the next poll-task tick would
                // also observe this, but a reader arriving first (e.g. an
                // HTTP request right at the deadline) must not see a
                // stale "still open" answer merely because the task
                // hasn't ticked yet.
                s_bb.window_open = false;
            }
        }
        xSemaphoreGive(s_bb.lock);
    } else {
        // Cannot safely read the window state -- fail toward "not active"
        // rather than risk telling a caller the bypass is live when the
        // read itself is unreliable. This is the ONE place in this module
        // where the fail-safe direction is "assume off," matching every
        // other security-relevant check in this codebase (e.g. ota_http.c's
        // own lock-timeout handling).
        ESP_LOGW(TAG, "boot_button_ota_bypass_active: internal lock timeout, reporting inactive");
    }
    return active;
}

uint32_t boot_button_bypass_remaining_ms(void)
{
    if (!s_bb.initialized || !s_bb.lock) {
        return 0;
    }
    uint32_t remaining = 0;
    if (xSemaphoreTake(s_bb.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_bb.window_open) {
            uint32_t elapsed = (uint32_t)(now_ms() - s_bb.window_opened_ms);
            if (elapsed < BOOT_BUTTON_WINDOW_MS) {
                remaining = BOOT_BUTTON_WINDOW_MS - elapsed;
            } else {
                s_bb.window_open = false; /* lazily close, same as boot_button_ota_bypass_active() */
            }
        }
        xSemaphoreGive(s_bb.lock);
    }
    return remaining;
}

void boot_button_close_bypass(const char *source)
{
    const char *src = source ? source : "unknown";
    if (!s_bb.initialized || !s_bb.lock) {
        return;
    }
    bool was_open = false;
    if (xSemaphoreTake(s_bb.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        was_open = s_bb.window_open;
        s_bb.window_open = false;
        xSemaphoreGive(s_bb.lock);
    } else {
        ESP_LOGE(TAG, "boot_button_close_bypass(%s): internal lock timeout -- could not confirm "
                      "the window is closed",
                 src);
        return;
    }
    if (was_open) {
        ESP_LOGW(TAG, "BOOT-BUTTON RECOVERY: bypass window closed early (%s)", src);
    } else {
        ESP_LOGI(TAG, "boot_button_close_bypass(%s): no window was open", src);
    }
}
