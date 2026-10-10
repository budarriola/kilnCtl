#include "task_harness.h"

#include <setjmp.h>
#include <string.h>

#include "hardware/gpio.h"
#include "semphr.h"

static TaskFunction_t      s_task_fn;
static jmp_buf             s_jmp;
static TickType_t          s_tick;
static uint32_t            s_pending;
static th_wait_hook_t      s_wait_hook;
static th_delay_hook_t     s_delay_hook;
static bool                s_pin_level[64];
static bool                s_irq_registered;
static unsigned            s_irq_pin;
static gpio_irq_callback_t s_irq_cb;
static bool                s_sem_take_fails;
static uint32_t            s_delay_calls;
static uint32_t            s_wait_calls;
static int                 s_dummy_task;
static int                 s_dummy_sem;

void th_reset(void)
{
    s_task_fn = NULL;
    s_tick = 0;
    s_pending = 0;
    s_wait_hook = NULL;
    s_delay_hook = NULL;
    memset(s_pin_level, 0, sizeof(s_pin_level));
    s_irq_registered = false;
    s_irq_pin = 0;
    s_irq_cb = NULL;
    s_sem_take_fails = false;
    s_delay_calls = 0;
    s_wait_calls = 0;
}

TaskFunction_t th_captured_task_fn(void) { return s_task_fn; }

void th_run_captured_task(void)
{
    if (s_task_fn == NULL) {
        return;
    }
    if (setjmp(s_jmp) == 0) {
        s_task_fn(NULL);
    }
}

void th_abort(void) { longjmp(s_jmp, 1); }

void       th_set_tick(TickType_t t) { s_tick = t; }
TickType_t th_get_tick(void) { return s_tick; }
void       th_advance_tick(TickType_t dt) { s_tick += dt; }

void th_set_wait_hook(th_wait_hook_t hook) { s_wait_hook = hook; }
void th_set_delay_hook(th_delay_hook_t hook) { s_delay_hook = hook; }

void th_gpio_set_level(unsigned pin, bool level)
{
    if (pin < sizeof(s_pin_level) / sizeof(s_pin_level[0])) {
        s_pin_level[pin] = level;
    }
}
bool     th_irq_registered(void) { return s_irq_registered; }
unsigned th_irq_pin(void) { return s_irq_pin; }
void     th_fire_drdy_isr(void)
{
    if (s_irq_cb != NULL) {
        s_irq_cb(s_irq_pin, GPIO_IRQ_EDGE_FALL);
    }
}
uint32_t th_pending_notifications(void) { return s_pending; }
void     th_set_sem_take_fails(bool fails) { s_sem_take_fails = fails; }
uint32_t th_delay_calls(void) { return s_delay_calls; }
uint32_t th_wait_calls(void) { return s_wait_calls; }

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words, void *arg,
                        UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)name;
    (void)stack_words;
    (void)arg;
    (void)priority;
    s_task_fn = fn;
    if (out_handle) {
        *out_handle = &s_dummy_task;
    }
    return pdPASS;
}

void vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t mask)
{
    (void)task;
    (void)mask;
}

TickType_t xTaskGetTickCount(void) { return s_tick; }

void vTaskDelayUntil(TickType_t *previous_wake_time, TickType_t time_increment)
{
    s_delay_calls++;
    *previous_wake_time += time_increment;
    s_tick = *previous_wake_time;
    if (s_delay_hook) {
        s_delay_hook();
    }
}

uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks_to_wait)
{
    s_wait_calls++;
    if (s_wait_hook) {
        s_wait_hook((uint32_t)ticks_to_wait);
    }
    uint32_t n = s_pending;
    if (clear_on_exit) {
        s_pending = 0;
    } else if (n > 0) {
        s_pending--;
    }
    if (n == 0) {
        s_tick += ticks_to_wait; // the wait ran out
    }
    return n;
}

void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{
    (void)task;
    s_pending++;
    if (woken) {
        *woken = pdFALSE;
    }
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_dummy_sem; }
BaseType_t        xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)sem;
    (void)ticks;
    return s_sem_take_fails ? pdFALSE : pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    (void)sem;
    return pdTRUE;
}

void gpio_init(uint pin) { (void)pin; }
void gpio_put(uint pin, bool value) { th_gpio_set_level(pin, value); }
void gpio_set_dir(uint pin, bool out)
{
    (void)pin;
    (void)out;
}
void gpio_pull_up(uint pin) { (void)pin; }
bool gpio_get(uint pin)
{
    return pin < sizeof(s_pin_level) / sizeof(s_pin_level[0]) ? s_pin_level[pin] : false;
}
void gpio_set_irq_enabled_with_callback(uint pin, uint32_t events, bool enabled,
                                         gpio_irq_callback_t callback)
{
    (void)events;
    s_irq_registered = enabled;
    s_irq_pin = pin;
    s_irq_cb = callback;
}
