#include "monitor_task.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "monitor_task";

static void monitor_task_entry(void *arg)
{
    monitor_task_t *monitor = (monitor_task_t *)arg;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << monitor->config.led_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    while (true) {
        if (monitor->config.task_handle != NULL &&
            *monitor->config.task_handle != NULL &&
            eTaskGetState(*monitor->config.task_handle) == eRunning) {
            gpio_set_level(monitor->config.led_gpio, 1);
            vTaskDelay(monitor->config.on_ticks);
            gpio_set_level(monitor->config.led_gpio, 0);
            vTaskDelay(monitor->config.off_ticks);
        } else {
            gpio_set_level(monitor->config.led_gpio, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(monitor->config.led_gpio, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
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

    return xTaskCreatePinnedToCore(monitor_task_entry,
                                   "monitor_task",
                                   2048,
                                   monitor,
                                   4,
                                   NULL,
                                   tskNO_AFFINITY);
}
