// recovery_keepalive.c -- see recovery_keepalive.h.
#include "recovery_keepalive.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "recovery_pico_proto.h"

static const char *TAG = "rec_keepalive";

#define KA_UART UART_NUM_1
#define KA_TX_GPIO 5
#define KA_RX_GPIO 4
#define KA_BAUD 230400
#define KA_RX_BUF 2048
#define KA_TASK_STACK_BYTES 3072

static SemaphoreHandle_t s_uart_lock; // held by the keepalive while writing, by the relay for a transfer
static StaticSemaphore_t s_uart_lock_storage;
static bool s_uart_ready;
static TaskHandle_t s_task;
static volatile uint32_t s_ka_sent;
static volatile uint32_t s_ka_send_errors;
static volatile int32_t s_ka_last_err;
static volatile uint32_t s_ka_last_send_ms;
static uint8_t s_payload[KILNLINK_FRAME_MAX_PAYLOAD];
static uint8_t s_wire[KILNLINK_FRAME_STUFFED_MAX];

static bool uart_setup(void)
{
    if (s_uart_ready) {
        return true;
    }
    uart_config_t cfg = {
        .baud_rate = KA_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(KA_UART, KA_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(KA_UART, &cfg) != ESP_OK ||
        uart_set_pin(KA_UART, KA_TX_GPIO, KA_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
            ESP_OK) {
        uart_driver_delete(KA_UART);
        return false;
    }
    s_uart_ready = true;
    return true;
}

static void fault_line_idle(void)
{
    // Latch the level BEFORE the pin becomes an output (hal_gpio_init_out()
    // contract in the application): no glitch, no floating gate on U1.
    gpio_reset_pin(RPP_FAULT_GPIO);
    gpio_set_level(RPP_FAULT_GPIO, RPP_FAULT_IDLE_LEVEL);
    if (gpio_set_direction(RPP_FAULT_GPIO, GPIO_MODE_OUTPUT) != ESP_OK) {
        ESP_LOGE(TAG, "could not drive GPIO%d as an output", RPP_FAULT_GPIO);
        return;
    }
    gpio_set_level(RPP_FAULT_GPIO, RPP_FAULT_IDLE_LEVEL);
    ESP_LOGI(TAG, "fault line GPIO%d driven %d (de-asserted)", RPP_FAULT_GPIO,
             RPP_FAULT_IDLE_LEVEL);
}

static void keepalive_task(void *arg)
{
    (void)arg;
    uint16_t msg_index = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RPP_KEEPALIVE_PERIOD_MS));
        uint32_t up_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (!rpp_keepalive_active(up_ms)) {
            // Bounded: a wedged image must let the Pico's S6b trip.
            ESP_LOGW(TAG, "keepalive window ended at %u ms; stack hwm %u bytes",
                     (unsigned)up_ms, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            s_task = NULL;
            vTaskDelete(NULL);
        }
        // Stand down while the relay owns the UART (its own frames keep the link up).
        if (xSemaphoreTake(s_uart_lock, 0) != pdTRUE) {
            continue;
        }
        if (s_uart_ready) {
            size_t pl = rpp_pack_keepalive(s_payload);
            size_t n = rpp_build_frame(msg_index++, s_payload, pl, s_wire, sizeof(s_wire));
            int32_t err = 0;
            if (n == 0) {
                err = -3;
            } else {
                int w = uart_write_bytes(KA_UART, s_wire, n);
                err = w < 0 ? -1 : ((size_t)w < n ? -2 : 0);
            }
            s_ka_last_send_ms = up_ms;
            s_ka_last_err = err;
            if (err == 0) {
                s_ka_sent++;
            } else {
                s_ka_send_errors++;
            }
            if (s_ka_sent == 1 || (s_ka_sent % 30u) == 0 || err != 0) { // boot log line, then ~1/min
                ESP_LOGI(TAG, "keepalive sent=%u errors=%u last_err=%d at %u ms", (unsigned)s_ka_sent,
                         (unsigned)s_ka_send_errors, (int)err, (unsigned)up_ms);
            }
            uart_flush_input(KA_UART); // Pico replies are not read here; never let them pile up
        }
        xSemaphoreGive(s_uart_lock);
    }
}

void recovery_keepalive_get_stats(recovery_keepalive_stats_t *out)
{
    out->sent = s_ka_sent;
    out->send_errors = s_ka_send_errors;
    out->last_err = s_ka_last_err;
    out->last_send_ms = s_ka_last_send_ms;
    out->task_running = s_task != NULL;
}

void recovery_keepalive_start(void)
{
    fault_line_idle();
    s_uart_lock = xSemaphoreCreateMutexStatic(&s_uart_lock_storage);
    if (!uart_setup()) {
        ESP_LOGE(TAG, "UART1 setup failed -- no link keepalive (Pico S6b applies as before)");
        return;
    }
    if (xTaskCreate(keepalive_task, "rec_keepalive", KA_TASK_STACK_BYTES, NULL, 3, &s_task) !=
        pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "could not start the keepalive task");
    }
}

bool recovery_keepalive_acquire(void)
{
    if (!s_uart_lock) {
        return false;
    }
    if (xSemaphoreTake(s_uart_lock, pdMS_TO_TICKS(3000)) != pdTRUE) {
        return false;
    }
    if (!uart_setup()) {
        xSemaphoreGive(s_uart_lock);
        return false;
    }
    uart_flush_input(KA_UART);
    return true;
}

void recovery_keepalive_release(void)
{
    uart_wait_tx_done(KA_UART, pdMS_TO_TICKS(200));
    uart_flush_input(KA_UART);
    xSemaphoreGive(s_uart_lock);
}
