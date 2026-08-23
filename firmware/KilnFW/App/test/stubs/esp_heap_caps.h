// Host-test stub -- see stubs/esp_err.h for why these exist. wifi_prov.c and
// uart_log_bridge.c each #include the real esp_heap_caps.h only to reach the
// MALLOC_CAP_* bit flags they OR into an xTaskCreatePinnedToCoreWithCaps()
// call site; neither host test calls that function (they drive the do_*()
// bodies directly, same pattern esp_timer.h's stub comment describes), so
// the flags only need to exist and be OR-able, not mean anything real here.
#ifndef TEST_STUB_ESP_HEAP_CAPS_H
#define TEST_STUB_ESP_HEAP_CAPS_H

#include <stdbool.h>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_8BIT   (1 << 1)
#define MALLOC_CAP_DEFAULT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// Added for safety_cfg_store.c's host test (test_safety_cfg_store.c): that
// file's real code calls the real esp_ptr_external_ram() to refuse a flash
// write from a task whose stack lives in PSRAM (2026-08-23 panic fix --
// esp_task_stack_is_sane_cache_disabled() aborts the whole board if a flash/
// NVS write is attempted from such a task; safety_cfg_store.c's guard turns
// that abort into a diagnosable ESP_ERR_INVALID_STATE instead). Deterministic
// here: a plain settable flag rather than a real pointer-range check, so a
// test can simulate "called from an external-RAM stack" without actually
// running on hardware or faking a PSRAM allocation. `static` (not extern):
// each translation unit that includes this header gets its own independent
// copy, same as every other file-scope stub state in this directory.
static bool s_stub_esp_ptr_external_ram = false;

static inline bool esp_ptr_external_ram(const void *p)
{
    (void)p;
    return s_stub_esp_ptr_external_ram;
}

static inline void esp_ptr_external_ram_test_set(bool external)
{
    s_stub_esp_ptr_external_ram = external;
}

#endif // TEST_STUB_ESP_HEAP_CAPS_H
