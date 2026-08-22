// Host-test stub -- see stubs/esp_err.h for why these exist. wifi_prov.c and
// uart_log_bridge.c each #include the real esp_heap_caps.h only to reach the
// MALLOC_CAP_* bit flags they OR into an xTaskCreatePinnedToCoreWithCaps()
// call site; neither host test calls that function (they drive the do_*()
// bodies directly, same pattern esp_timer.h's stub comment describes), so
// the flags only need to exist and be OR-able, not mean anything real here.
#ifndef TEST_STUB_ESP_HEAP_CAPS_H
#define TEST_STUB_ESP_HEAP_CAPS_H

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_8BIT   (1 << 1)
#define MALLOC_CAP_DEFAULT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#endif // TEST_STUB_ESP_HEAP_CAPS_H
