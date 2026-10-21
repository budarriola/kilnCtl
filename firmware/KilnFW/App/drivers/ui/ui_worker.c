#include "ui_worker.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "stack_margin.h"

static const char *TAG = "ui_worker";

/* Largest of the former per-tap stacks (lcd_pin_ui). */
#define UI_WORKER_STACK_BYTES 6144
#define UI_WORKER_QUEUE_LEN 6

typedef struct {
    ui_worker_fn_t fn;
    void *arg;
} ui_worker_job_t;

static StaticTask_t s_tcb;
/* ESP-IDF: StackType_t is a byte, stack depth is in bytes. */
static StackType_t s_stack[UI_WORKER_STACK_BYTES];
static StaticQueue_t s_queue_storage;
static uint8_t s_queue_area[UI_WORKER_QUEUE_LEN * sizeof(ui_worker_job_t)];
static QueueHandle_t s_queue;
static TaskHandle_t s_task;

static void ui_worker_task(void *unused)
{
    (void)unused;
    for (;;) {
        ui_worker_job_t job;
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) == pdTRUE && job.fn != NULL) {
            job.fn(job.arg);
        }
    }
}

/* Every caller runs on the LVGL task (tap handlers), so first-use start needs no lock. */
static bool ensure_started(void)
{
    if (s_queue != NULL) return true;
    QueueHandle_t q = xQueueCreateStatic(UI_WORKER_QUEUE_LEN, sizeof(ui_worker_job_t), s_queue_area, &s_queue_storage);
    if (q == NULL) return false;
    s_queue = q;
    TaskHandle_t t = xTaskCreateStatic(ui_worker_task, "ui_worker", UI_WORKER_STACK_BYTES, NULL, 5,
                                       s_stack, &s_tcb);
    if (t == NULL) {
        ESP_LOGE(TAG, "worker task create failed");
        s_queue = NULL;
        return false;
    }
    s_task = t;
    (void)stack_margin_register("ui_worker", &s_task, UI_WORKER_STACK_BYTES);
    return true;
}

static bool submit_ex(ui_worker_fn_t fn, void *arg, bool front)
{
    if (fn == NULL || !ensure_started()) {
        ESP_LOGW(TAG, "submit refused (fn=%p)", (void *)fn);
        return false;
    }
    ui_worker_job_t job = {.fn = fn, .arg = arg};
    BaseType_t ok = front ? xQueueSendToFront(s_queue, &job, 0) : xQueueSend(s_queue, &job, 0);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "queue full");
        return false;
    }
    return true;
}

bool ui_worker_submit(ui_worker_fn_t fn, void *arg)
{
    return submit_ex(fn, arg, false);
}

bool ui_worker_submit_front(ui_worker_fn_t fn, void *arg)
{
    return submit_ex(fn, arg, true);
}
