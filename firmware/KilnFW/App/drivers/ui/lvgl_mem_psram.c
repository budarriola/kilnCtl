// lvgl_mem_psram -- routes every LVGL allocation to PSRAM.
//
// Why this file exists
// --------------------
// LVGL allocates a great many small, long-lived objects: every lv_obj, style,
// label buffer and event descriptor on every page. With
// CONFIG_LV_USE_CLIB_MALLOC those went through plain malloc(), and because
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL forces allocations below its threshold
// into internal RAM, all of them landed in the internal DRAM heap.
//
// Measured on this board (main.c logs the figures at boot, uart_bridge_ext.c
// at each bridge-task creation): the largest contiguous DRAM block is 163840
// bytes 1.4s into boot, and ~2560 bytes by 2.9s -- the window in which the UI
// is constructed. Nothing leaks; the heap is simply shredded into pieces too
// small to satisfy any multi-KB request afterwards. That is what stopped the
// 8KB LVGL task stack, the 3KB PC-link safety watchdog stack and the UART
// inbox queues from being created at all, each of which had to be moved to
// PSRAM individually to keep the firmware booting.
//
// Moving the *fragmenter* is the fix those individual moves were working
// around. LVGL is an ideal candidate: its allocations are numerous but not
// latency-critical at these sizes, its draw buffers already live in PSRAM
// (lvgl_port.c), and nothing it allocates here is touched from an ISR or
// needs to be DMA-capable.
//
// Two rejected alternatives, both tried on hardware and reverted (see
// sdkconfig.defaults for the full record):
//
//   1. Global allocator knobs -- lowering SPIRAM_MALLOC_ALWAYSINTERNAL and
//      enabling SPIRAM_TRY_ALLOCATE_WIFI_LWIP. These change which pool SMALL
//      allocations come from and do nothing for a large contiguous request;
//      on hardware they made matters worse and the display stopped starting.
//   2. LVGL's built-in pool allocator (LV_USE_BUILTIN_MALLOC). Right instinct
//      -- confine LVGL to one arena -- but with LV_MEM_ADR=0 the pool is a
//      static array that lands in .bss, i.e. still DRAM, on top of everything
//      else. It left the board completely unresponsive.
//
// LV_STDLIB_CUSTOM avoids both traps: no fixed pool to size wrongly, no
// static array, and the allocations go to the PSRAM heap that already has
// ~8MB free.
//
// Contract
// --------
// Selected by CONFIG_LV_USE_CUSTOM_MALLOC (mapped to LV_STDLIB_CUSTOM in
// components/lvgl/src/lv_conf_kconfig.h). With that set LVGL compiles none of
// its own allocator cores, so these symbols MUST be provided or the link
// fails -- the set below mirrors components/lvgl/src/stdlib/clib/
// lv_mem_core_clib.c exactly, which is the reference implementation for this
// contract.
/* sdkconfig.h MUST come before lvgl.h. LVGL's lv_conf_internal.h derives
 * LV_USE_STDLIB_MALLOC from CONFIG_LV_USE_CUSTOM_MALLOC (via
 * lv_conf_kconfig.h); with that macro undefined it silently falls back to
 * LV_STDLIB_BUILTIN, the guard below evaluates false, and this file compiles
 * to an empty object. The link then fails with "undefined reference to
 * lv_mem_init/lv_malloc_core/..." from inside liblvgl.a, which points at
 * LVGL rather than at the real cause -- worth the explicit include and this
 * comment, because the failure gives no hint that an include order is to
 * blame. */
#include "sdkconfig.h"

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "lvgl_mem";

/* MALLOC_CAP_8BIT alongside MALLOC_CAP_SPIRAM: LVGL stores byte-addressable
 * data (label text, style properties), so 32-bit-only memory is not a valid
 * backing store for it. */
#define LVGL_MEM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Falling back to the default heap when PSRAM is exhausted is deliberate.
 * The point of this module is to keep LVGL OUT of internal DRAM in the normal
 * case, not to guarantee it never touches it: a UI that degrades under
 * extreme memory pressure is better than one that fails to draw. The warning
 * is rate-limited to once per boot because if it ever fires it will fire a
 * great many times, and the flood would itself become the problem (this
 * codebase has been bitten by exactly that three times -- see
 * lvgl_port.c's touch_read_cb() and NS2009.c). */
static bool s_fallback_warned;

static void *lvgl_mem_alloc(size_t size)
{
    void *p = heap_caps_malloc(size, LVGL_MEM_CAPS);
    if (p) {
        return p;
    }

    if (!s_fallback_warned) {
        s_fallback_warned = true;
        ESP_LOGW(TAG, "PSRAM allocation of %u bytes failed -- falling back to the default heap "
                      "(further occurrences not logged)", (unsigned)size);
    }
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void lv_mem_init(void)
{
    /* Nothing to initialise: this backs onto the ESP-IDF heap, which is up
     * long before LVGL is. Same no-op as the clib core. */
    return;
}

void lv_mem_deinit(void)
{
    return;
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    /* Not supported, same as the clib core: this allocator has no pool of its
     * own to extend -- it draws from the PSRAM heap, which is already as
     * large as the hardware allows. */
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
    return;
}

void *lv_malloc_core(size_t size)
{
    return lvgl_mem_alloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    /* heap_caps_realloc() handles a NULL p as a plain allocation and honours
     * the caps for the new block, including when it has to move an existing
     * one -- so a block that fell back to internal memory can migrate to
     * PSRAM on a later grow, rather than being pinned there forever. */
    void *out = heap_caps_realloc(p, new_size, LVGL_MEM_CAPS);
    if (out || new_size == 0) {
        return out;
    }
    return heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    /* Correct for both branches above: heap_caps_free() dispatches on the
     * region a pointer actually came from, so PSRAM and fallback internal
     * allocations are both released properly. */
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    /* Not supported, same as the clib core. LVGL's monitor describes a
     * self-managed pool; this allocator has none, and the numbers that
     * actually matter on this board (DRAM vs PSRAM free, largest contiguous
     * block) are already logged by main.c at boot and by uart_bridge_ext.c at
     * task creation. Reporting a partially-filled struct here would be worse
     * than reporting nothing. */
    LV_UNUSED(mon_p);
    return;
}

lv_result_t lv_mem_test_core(void)
{
    /* Nothing to integrity-check without a self-managed pool; the ESP-IDF
     * heap has its own poisoning/integrity facilities for that. */
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
