#ifndef MONITOR_TASK_H
#define MONITOR_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"

typedef struct {
    TaskHandle_t *task_handle;
    int led_gpio; /* hal_gpio pin number; -1 == no heartbeat LED, see monitor_task.c */
    TickType_t on_ticks;
    TickType_t off_ticks;
} monitor_task_config_t;

typedef struct {
    monitor_task_config_t config;
} monitor_task_t;

void monitor_task_init(monitor_task_t *monitor, TaskHandle_t *task_handle);
BaseType_t monitor_task_start(monitor_task_t *monitor);

#endif // MONITOR_TASK_H
