// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> safety_link.h, which
// embeds a uart_owner_t BY VALUE in SafetyLinkClass. Only a complete type is
// needed (SafetyLinkClass is never instantiated by this test, only its
// definition is parsed), so a dummy field is enough.
#ifndef TEST_STUB_UART_OWNER_H
#define TEST_STUB_UART_OWNER_H

#include <stddef.h>
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/task.h"

/* task_handle/event_task_handle added for safety_link.c's host build
 * (App/test/test_safety_link_compile.c) -- safety_link.c's own init/deinit
 * paths read/write these two fields directly (see the real espInterfaces/
 * uart_owner.h for the field this mirrors), everything else about this
 * struct is still an opaque stand-in nothing else here inspects. */
typedef struct {
    int _unused;
    TaskHandle_t task_handle;
    TaskHandle_t event_task_handle;
} uart_owner_t;

/* Declarations only -- App/test/test_safety_link_compile.c supplies fake
 * bodies (same convention as its profile_executor_get_status()/kiln_io_*
 * fakes), since the real espInterfaces/uart_owner.c drives actual ESP-IDF
 * UART hardware and cannot run/link off-target. */
esp_err_t uart_owner_init(uart_owner_t *owner, uart_port_t port, int tx_io, int rx_io,
                           int baud_rate, unsigned queue_len, unsigned task_priority,
                           uint32_t stack_depth, int core_id);
esp_err_t uart_owner_deinit(uart_owner_t *owner);
uint32_t uart_owner_get_rx_error_count(const uart_owner_t *owner);
esp_err_t uart_owner_transfer(uart_owner_t *owner, const uint8_t *tx_buffer, size_t tx_length,
                               uint8_t *rx_buffer, size_t rx_length, size_t *rx_length_out,
                               uint32_t timeout_ms);

#endif // TEST_STUB_UART_OWNER_H
