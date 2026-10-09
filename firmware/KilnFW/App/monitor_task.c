#include "monitor_task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "hal_gpio.h"
#include "rtc_watchdog.h"
#include "crash_report.h" /* crash_report_note_alive() -- same "prove the scheduler ran this
                             * cycle" write as rtc_watchdog_feed() below, RTC memory only, no
                             * NVS/flash touch, safe from this task's PSRAM stack. */

static const char *TAG = "monitor_task";

static void monitor_task_entry(void *arg)
{
    monitor_task_t *monitor = (monitor_task_t *)arg;

    /* The kilnCtl main board has no MCU-driven LED (D25/D28 are rail
     * indicators wired straight to the 3.3V/5V rails) and the dev board's
     * addressable RGB LED is removed on this build, so HEARTBEAT_LED_GPIO is
     * -1 by default. Configuring GPIO -1 would abort the boot in
     * gpio_config(); instead the task keeps running with the same cadence and
     * reports over the log link, which is the only place anybody is watching
     * anyway. */
    bool have_led = (monitor->config.led_gpio >= 0);

    if (have_led) {
        /* hal_gpio_init_out() latches the idle level (off/low) BEFORE
         * switching direction -- see hal_gpio.h's latch-before-direction
         * contract. Not treated as fatal: that would abort/reboot a kiln
         * controller mid-firing because an *indicator LED* would not
         * configure. Degrade to the no-LED path instead -- the heartbeat
         * still reports over the log link, which is where anyone is
         * actually watching. */
        hal_status_t st = hal_gpio_init_out(monitor->config.led_gpio, false);
        if (st != HAL_OK) {
            ESP_LOGE(TAG, "heartbeat LED gpio %d config failed: %s -- reporting over the log link "
                          "instead", monitor->config.led_gpio, hal_status_to_name(st));
            have_led = false;
        }
    }

    if (!have_led) {
        ESP_LOGI(TAG, "no heartbeat LED configured; reporting over the log link instead");
    }

    /* One log line per this many heartbeat cycles when there is no LED. At the
     * ~2 Hz "watched task alive" cadence that is roughly one line a minute --
     * enough to prove the scheduler is running without flooding the link. */
    const uint32_t log_every = 60;
    uint32_t cycles = 0;

    while (true) {
        /* "Alive" is anything but deleted/invalid, NOT eRunning. eRunning means
         * "currently executing on a core", which for a task that spends its
         * life blocked on a queue is almost never true -- testing for it made
         * every healthy watched task read as dead, and made the one state worth
         * catching (the task actually gone) indistinguishable from the normal
         * case. */
        eTaskState state = eInvalid;
        if (monitor->config.task_handle != NULL && *monitor->config.task_handle != NULL) {
            state = eTaskGetState(*monitor->config.task_handle);
        }
        const bool alive = (state != eInvalid && state != eDeleted);
        const TickType_t on = alive ? monitor->config.on_ticks : pdMS_TO_TICKS(500);
        const TickType_t off = alive ? monitor->config.off_ticks : pdMS_TO_TICKS(500);

        /* rtc_watchdog.h: this feed is what proves the scheduler itself is
         * still alive, independent of whether the ONE task this monitor
         * watches (monitor->config.task_handle) is. A hang that stops the
         * scheduler from ever running THIS task -- not just the watched one
         * -- is exactly the lockup the RTC watchdog exists to catch; a feed
         * hook on a task that only runs when its watched target is also
         * healthy would defeat that. This runs every cycle (every
         * on+off, ~300 ms with the default heartbeat timing), comfortably
         * inside RTC_WATCHDOG_TIMEOUT_MS's 20 s margin. */
        rtc_watchdog_feed();
        crash_report_note_alive();

        if (have_led) {
            hal_gpio_set(monitor->config.led_gpio, true);
            vTaskDelay(on);
            hal_gpio_set(monitor->config.led_gpio, false);
            vTaskDelay(off);
        } else {
            vTaskDelay(on + off);
            if (++cycles >= log_every) {
                cycles = 0;
                ESP_LOGI(TAG, "heartbeat: watched task %s", alive ? "running" : "not running");
            }
        }
    }
}

void monitor_task_init(monitor_task_t *monitor, TaskHandle_t *task_handle)
{
    if (!monitor) {
        return;
    }

    monitor->config.task_handle = task_handle;
    monitor->config.led_gpio = HEARTBEAT_LED_GPIO;
    monitor->config.on_ticks = pdMS_TO_TICKS(150);
    monitor->config.off_ticks = pdMS_TO_TICKS(150);
}

BaseType_t monitor_task_start(monitor_task_t *monitor)
{
    if (!monitor) {
        return pdFALSE;
    }

    /* 3072, not 2048: every heartbeat line goes through ESP_LOGx, whose
     * vsnprintf and the log bridge's own formatting both run on this stack.
     * 2048 left very little headroom above that for a task whose entire job is
     * to prove the scheduler is still healthy.
     *
     * 2026-08-22: moved off internal SRAM. This task never touches flash/NVS
     * (no profile/zones/wifi_prov calls anywhere in monitor_task_entry() above)
     * and only ever does gpio_set_level()/vTaskDelay()/ESP_LOGI -- none of
     * which are ISR context or DMA-dependent -- so it has none of the hazards
     * documented in uart_bridge_ext.c (PSRAM-stack-during-cache-disable) that
     * keep the flash-touching bridge tasks' own stacks internal. It is one of
     * ~20 remaining plain-internal-stack tasks contending with the WiFi
     * driver's own internal-SRAM buffer storm during the same boot window
     * uart_bridge_ext.c's retry_task_create_pinned() comment documents; moving
     * it (and heartbeat_LED indicator work is not latency-critical) frees its
     * 3072 B for that window without touching anything hardware-DMA-facing. */
    return xTaskCreatePinnedToCoreWithCaps(monitor_task_entry,
                                            "monitor_task",
                                            3072,
                                            monitor,
                                            4,
                                            NULL,
                                            tskNO_AFFINITY,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
