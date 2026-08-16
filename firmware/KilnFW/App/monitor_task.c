#include "monitor_task.h"

#include "esp_log.h"
#include "esp_err.h"

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
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << (int)monitor->config.led_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        /* Not ESP_ERROR_CHECK: that aborts, and abort() here would reboot a
         * kiln controller mid-firing because an *indicator LED* would not
         * configure. Degrade to the no-LED path instead -- the heartbeat still
         * reports over the log link, which is where anyone is actually
         * watching. */
        esp_err_t err = gpio_config(&io_conf);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "heartbeat LED gpio %d config failed: %s -- reporting over the log link "
                          "instead", (int)monitor->config.led_gpio, esp_err_to_name(err));
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

        if (have_led) {
            gpio_set_level(monitor->config.led_gpio, 1);
            vTaskDelay(on);
            gpio_set_level(monitor->config.led_gpio, 0);
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
    monitor->config.led_gpio = (gpio_num_t)HEARTBEAT_LED_GPIO;
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
     * to prove the scheduler is still healthy. */
    return xTaskCreatePinnedToCore(monitor_task_entry,
                                   "monitor_task",
                                   3072,
                                   monitor,
                                   4,
                                   NULL,
                                   tskNO_AFFINITY);
}
