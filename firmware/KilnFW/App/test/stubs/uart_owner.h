// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// so backup_http.c's host test can reach ota_http.h -> safety_link.h, which
// embeds a uart_owner_t BY VALUE in SafetyLinkClass. Only a complete type is
// needed (SafetyLinkClass is never instantiated by this test, only its
// definition is parsed), so a dummy field is enough.
#ifndef TEST_STUB_UART_OWNER_H
#define TEST_STUB_UART_OWNER_H

typedef struct {
    int _unused;
} uart_owner_t;

#endif // TEST_STUB_UART_OWNER_H
