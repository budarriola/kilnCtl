// FD-02 glue: wires firing_trace.c's IO seam to cfg_fs and the flash worker. ESP-only (host tests
// drive firing_trace.c through their own io table). See firing_trace.h.
#include "firing_trace_glue.h"

#include "esp_heap_caps.h"

#include "cfg_fs.h"
#include "firing_trace.h"
#include "flash_hop.h"
#include "flash_worker.h"

/* The trace buffer lives in PSRAM only: never fall back to internal RAM (8 KB floor). */
static void *ft_alloc_psram(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static esp_err_t ft_run(void (*fn)(void *arg), void *arg)
{
    return flash_hop_run(fn, arg);
}

static esp_err_t ft_post(void (*fn)(void *arg))
{
    return uart_bridge_ext_post_trace_on_flash_worker(fn); /* FY-01: own slot, never the safety recapture's */
}

void firing_trace_glue_init(void)
{
    static const firing_trace_io_t io = {
        .write = cfg_fs_write_atomic,
        .append = cfg_fs_append,
        .read = cfg_fs_read,
        .post = ft_post,
        .run = ft_run,
        .alloc = ft_alloc_psram,
    };
    if (!cfg_fs_is_available()) {
        return; /* no cfg partition: the trace stays disabled, nothing else changes */
    }
    firing_trace_init(&io);
}
