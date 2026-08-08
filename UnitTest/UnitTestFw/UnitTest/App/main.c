/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "AD9833.h"
#include "DcDac.h"
#include "SSD1306.h"
#include "i2c_scan.h"
#include "monitor_task.h"
#include "settings.h"
#include "uart_bridge.h"
#include "uart_log_bridge.h"
#include "uart_owner.h"
#include "uart_protocol.h"

static const char *TAG = "app_main";

/* Every task creation in this file is a single-line "start" call -- the
 * semaphore/queue/task choreography behind each one lives in that driver's
 * own file (DcDac_start in DcDac.c, monitor_task_start in monitor_task.c,
 * uart_bridge_start_* in uart_bridge.c, etc.), not here. */
void app_main(void)
{
    // Installed before anything else touches ESP_LOGx, so every line from
    // here on -- including failures during the driver bring-up immediately
    // below, e.g. SSD1306_start() -- is captured and queued. Nothing is
    // actually sent yet (there's no uart_protocol_t until further down); see
    // uart_log_bridge_start() below for when the backlog actually flushes.
    uart_log_bridge_early_init();

    static DcDacClass dac;
    esp_err_t dac_err = DcDac_start(&dac);
    if (dac_err != ESP_OK) {
        ESP_LOGE(TAG, "DAC/I2C bring-up failed: %s", esp_err_to_name(dac_err));
        return;
    }

    // Runs before anything else touches the bus (including SSD1306_start
    // immediately below), so its results reflect what's actually wired up
    // rather than a bus already mid-transaction with the OLED -- and if the
    // OLED doesn't come up, the scan results (logged here, forwarded like
    // any other ESP_LOGx -- see uart_log_bridge.h) show up right alongside
    // that failure in the GUI's Device Console.
    i2c_scan_bus(dac.bus);

    // SSD1306 OLED shares the DAC's already-created I2C bus (a second
    // device on the same physical bus, per SSD1306_init's contract). Not as
    // critical as the DAC/UART path above, so a failure here is logged but
    // does not abort app_main.
    static SSD1306Class oled;
    esp_err_t oled_err = SSD1306_start(&oled, dac.bus);

    static monitor_task_t monitor;
    monitor_task_init(&monitor, &dac.owner.task_handle);
    if (monitor_task_start(&monitor) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat monitor task");
    }

    static AD9833Class ad9833_gen;
    esp_err_t ad9833_err = AD9833_init(&ad9833_gen, AD9833_SPI_HOST, AD9833_SCLK_IO,
                                        AD9833_MOSI_IO, AD9833_CS_IO, AD9833_MCLK_HZ);
    if (ad9833_err != ESP_OK) {
        ESP_LOGE(TAG, "AD9833_init failed: %s", esp_err_to_name(ad9833_err));
    }

    static uart_owner_t uart_owner;
    esp_err_t uart_err = uart_owner_init(&uart_owner, UART_OWNER_PORT_NUM, UART_OWNER_TX_IO,
                                          UART_OWNER_RX_IO, UART_OWNER_BAUD_RATE,
                                          UART_OWNER_QUEUE_LEN, UART_OWNER_TASK_PRIORITY,
                                          UART_OWNER_STACK_SIZE, tskNO_AFFINITY);
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_owner_init failed: %s", esp_err_to_name(uart_err));
        return;
    }

    static uart_protocol_t uart_proto;
    uart_err = uart_protocol_init(&uart_proto, &uart_owner, UART_PROTO_DEVICE_ESP,
                                   UART_PROTOCOL_TASK_PRIORITY, UART_PROTOCOL_STACK_SIZE,
                                   tskNO_AFFINITY);
    if (uart_err != ESP_OK) {
        ESP_LOGE(TAG, "uart_protocol_init failed: %s", esp_err_to_name(uart_err));
        return;
    }

    // Starts draining the backlog (everything logged since app_main started)
    // over the wire. Started before the other bridge tasks below purely so
    // buffered boot-time log lines -- e.g. an SSD1306_start() failure --
    // reach the PC as early as possible; registration order otherwise
    // doesn't matter between these tasks.
    if (uart_log_bridge_start(&uart_proto) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start log uart bridge task");
    }

    if (uart_bridge_start_dac_task(&uart_proto, &dac) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DAC uart bridge task");
    }
    if (ad9833_err == ESP_OK && uart_bridge_start_ad9833_task(&uart_proto, &ad9833_gen) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AD9833 uart bridge task");
    }
    if (uart_bridge_start_info_task(&uart_proto) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start info uart bridge task");
    }
    if (uart_bridge_start_system_task(&uart_proto, &uart_owner) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start system uart bridge task");
    }
    if (oled_err == ESP_OK && uart_bridge_start_oled_task(&uart_proto, &oled) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start OLED uart bridge task");
    }
}
