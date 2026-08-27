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

typedef struct {
    int _unused;
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

#endif // TEST_STUB_UART_PROTOCOL_H
