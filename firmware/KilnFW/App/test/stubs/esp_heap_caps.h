// Host-test stub -- see stubs/esp_err.h for why these exist. wifi_prov.c and
// uart_log_bridge.c each #include the real esp_heap_caps.h only to reach the
// MALLOC_CAP_* bit flags they OR into an xTaskCreatePinnedToCoreWithCaps()
// call site; neither host test calls that function (they drive the do_*()
// bodies directly, same pattern esp_timer.h's stub comment describes), so
// the flags only need to exist and be OR-able, not mean anything real here.
#ifndef TEST_STUB_ESP_HEAP_CAPS_H
#define TEST_STUB_ESP_HEAP_CAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1 << 0)
#define MALLOC_CAP_8BIT   (1 << 1)
// 2026-09-08 firing-history stack-overflow fix: the firing_stats read path
// allocates its 1364 B blobs from INTERNAL DRAM (it reaches NVS/flash, so
// a PSRAM buffer would be the wrong pool). Caps are ignored on the host.
#define MALLOC_CAP_INTERNAL (1 << 2)
#define MALLOC_CAP_DEFAULT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// 2026-08-31 httpd_worker stack-overflow fix: zones_get_handler() (and
// several dashboard_http.c handlers, which cannot compile on the host --
// see dashboard_json.h's own note) now call the real heap_caps_malloc()
// instead of a plain malloc(), so their response buffers land in PSRAM
// rather than the internal DRAM this board is documented to exhaust. This
// stub actually allocates (via the host's real malloc(), caps ignored --
// the host has no PSRAM/internal-DRAM distinction to model) so the handler
// under test gets a real, usable buffer, but the fail flag below lets a
// test simulate the board being out of that pool and exercise the
// handler's NULL-check-and-500 path deterministically, the same
// s_stub_esp_ptr_external_ram-style pattern this file already uses for
// safety_cfg_store.c's PSRAM-stack guard test. `static` (not extern): each
// translation unit gets its own independent copy. free() is the host's
// real free() -- heap_caps_malloc()'s host allocation and its handler's
// free(json) are symmetric, same as on target.
static bool s_stub_heap_caps_malloc_fail = false;

static inline void heap_caps_malloc_test_set_fail(bool fail)
{
    s_stub_heap_caps_malloc_fail = fail;
}

// 2026-09-08 firing-history stack-overflow fix: a test needs to assert that a
// code path allocates its large buffers rather than stacking them, and the
// fail-flag above cannot distinguish "one of four frames still allocates" from
// "all four do" (any single failure short-circuits the whole read). This
// counter, sampled around a call, gives that resolution -- a frame moved back
// onto the stack shows up as one fewer allocation. Counts only allocations
// >= min_bytes so incidental small allocations elsewhere cannot mask a
// regression. `static` per translation unit, same as every other stub state.
static unsigned s_stub_heap_caps_malloc_count = 0;
static size_t s_stub_heap_caps_malloc_count_min_bytes = 0;

static inline void heap_caps_malloc_test_reset_count(size_t min_bytes)
{
    s_stub_heap_caps_malloc_count = 0;
    s_stub_heap_caps_malloc_count_min_bytes = min_bytes;
}

static inline unsigned heap_caps_malloc_test_count(void)
{
    return s_stub_heap_caps_malloc_count;
}

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    if (s_stub_heap_caps_malloc_fail) {
        return NULL;
    }
    if (size >= s_stub_heap_caps_malloc_count_min_bytes) {
        s_stub_heap_caps_malloc_count++;
    }
    return malloc(size);
}

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
