// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> safety_link.h, which
// embeds a uart_protocol_t BY VALUE in SafetyLinkClass. Only a complete type
// is needed (SafetyLinkClass is never instantiated by this test, only its
// definition is parsed), so a dummy field is enough.
//
// uart_proto_message_t is also embedded BY VALUE (safety_link.h's
// stashed_config_page), so it needs the same treatment -- field layout
// mirrors the real espInterfaces/uart_protocol.h so sizeof/memcpy usage in
// safety_link.c still makes sense, but nothing here is exercised by the
// tests that pull this stub in.
#ifndef TEST_STUB_UART_PROTOCOL_H
#define TEST_STUB_UART_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "uart_owner.h"

/* rx_task_handle added for safety_link.c's host build, same reasoning as
 * uart_owner.h's task_handle/event_task_handle -- safety_link.c reads it
 * directly. */
typedef struct {
    int _unused;
    TaskHandle_t rx_task_handle;
} uart_protocol_t;

#define UART_PROTO_MAX_PAYLOAD 253

typedef enum {
    UART_PROTO_DEVICE_ESP = 0,
    UART_PROTO_DEVICE_HOST = 1,
    UART_PROTO_DEVICE_SAFETY = 2,
} uart_proto_device_t;

typedef struct {
    uart_proto_device_t device;
    uint8_t task_id;
    uint16_t msg_index;
    uint8_t length;
    uint8_t payload[UART_PROTO_MAX_PAYLOAD];
} uart_proto_message_t;

/* Declarations only, for safety_link.c's host build -- App/test/
 * test_safety_link_compile.c supplies fake bodies, since the real
 * espInterfaces/uart_protocol.c drives an actual UART owner and cannot
 * run/link off-target. Signatures mirror the real header. */
esp_err_t uart_protocol_init(uart_protocol_t *proto, uart_owner_t *owner,
                              uart_proto_device_t own_device, unsigned task_priority,
                              uint32_t stack_depth, int core_id);
esp_err_t uart_protocol_deinit(uart_protocol_t *proto);
esp_err_t uart_protocol_register_task(uart_protocol_t *proto, uint8_t task_id,
                                       unsigned inbox_len, QueueHandle_t *out_inbox);
esp_err_t uart_protocol_unregister_task(uart_protocol_t *proto, uint8_t task_id);
esp_err_t uart_protocol_get_task_broadcast_dropped(uart_protocol_t *proto, uint8_t task_id,
                                                    uint32_t *out);
esp_err_t uart_protocol_get_deframe_stats(uart_protocol_t *proto, uint32_t *out_frames_deframed,
                                          uint32_t *out_frames_routed_nowhere,
                                          uint32_t *out_frame_length_mismatch,
                                          uint32_t *out_frame_crc_mismatch,
                                          uint32_t *out_frame_resync);
esp_err_t uart_protocol_receive(QueueHandle_t inbox, uart_proto_message_t *out_msg, TickType_t wait_ticks);
esp_err_t uart_protocol_send(uart_protocol_t *proto, uart_proto_device_t dst_device,
                              uint8_t dst_task, uint8_t src_task, const uint8_t *payload,
                              size_t length, uint32_t ack_timeout_ms);
esp_err_t uart_protocol_send_broadcast(uart_protocol_t *proto, uart_proto_device_t dst_device,
                                        uint8_t dst_task, uint8_t src_task,
                                        const uint8_t *payload, size_t length);

#endif // TEST_STUB_UART_PROTOCOL_H
