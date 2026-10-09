#include "dram_watch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "hal_time.h"

#define DRAM_WATCH_PERIOD_US (2 * 1000 * 1000)

/* Regions the one-shot alarm dump summarises. The ESP32-S3's internal heap is
 * a handful of regions (DRAM, D/IRAM, RTC fast); a region beyond this many is
 * counted in the totals line but not listed, and the dump says so. */
#define DRAM_WATCH_DUMP_REGIONS 8

static const char *TAG = "dram_watch";

static dram_watch_t s_w;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;

/* Runs on the shared esp_timer task (CONFIG_ESP_TIMER_TASK_STACK_SIZE, 3584 B
 * here). Deliberately does no logging and no heap walk-and-print: it takes one
 * sample and records it, and dram_watch_service() does the rest from a task. */
static void sample_cb(void *arg)
{
    (void)arg;
    /* Same capability mask /api/status reports, so the two are comparable. */
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t up_s = (uint32_t)(hal_time_now_us() / 1000000);
    portENTER_CRITICAL(&s_mux);
    (void)dram_watch_update(&s_w, largest, up_s);
    portEXIT_CRITICAL(&s_mux);
}

void dram_watch_start(void)
{
    if (s_timer != NULL) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    dram_watch_init(&s_w);
    portEXIT_CRITICAL(&s_mux);
    const esp_timer_create_args_t args = {.callback = sample_cb, .arg = NULL, .name = "dram_watch"};
    if (esp_timer_create(&args, &s_timer) != ESP_OK) {
        s_timer = NULL;
        ESP_LOGE(TAG, "esp_timer_create failed; largest-block low-water mark unavailable");
        return;
    }
    sample_cb(NULL);
    if (esp_timer_start_periodic(s_timer, DRAM_WATCH_PERIOD_US) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed; only the first sample was taken");
    }
}

bool dram_watch_get(size_t *low_largest, uint32_t *low_at_s)
{
    bool valid;
    portENTER_CRITICAL(&s_mux);
    valid = s_w.valid;
    if (valid) {
        if (low_largest) {
            *low_largest = s_w.low_largest;
        }
        if (low_at_s) {
            *low_at_s = s_w.low_at_s;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return valid;
}

typedef struct {
    intptr_t start;
    intptr_t end;
    size_t free_bytes;
    size_t largest_free;
    uint16_t free_blocks;
    uint16_t used_blocks;
} dram_watch_region_t;

typedef struct {
    dram_watch_region_t r[DRAM_WATCH_DUMP_REGIONS];
    unsigned n;
    unsigned unlisted;
} dram_watch_dump_t;

/* Runs with the region's heap lock held: arithmetic only, never a log call. */
static bool dump_walker(walker_heap_into_t heap, walker_block_info_t block, void *user)
{
    dram_watch_dump_t *d = (dram_watch_dump_t *)user;
    dram_watch_region_t *cur = (d->n > 0 && d->r[d->n - 1].start == heap.start) ? &d->r[d->n - 1] : NULL;
    if (cur == NULL) {
        if (d->n >= DRAM_WATCH_DUMP_REGIONS) {
            d->unlisted++;
            return false; /* skip the rest of this region */
        }
        cur = &d->r[d->n++];
        cur->start = heap.start;
        cur->end = heap.end;
        cur->free_bytes = 0;
        cur->largest_free = 0;
        cur->free_blocks = 0;
        cur->used_blocks = 0;
    }
    if (block.used) {
        if (cur->used_blocks < UINT16_MAX) {
            cur->used_blocks++;
        }
    } else {
        cur->free_bytes += block.size;
        if (block.size > cur->largest_free) {
            cur->largest_free = block.size;
        }
        if (cur->free_blocks < UINT16_MAX) {
            cur->free_blocks++;
        }
    }
    return true;
}

static void dump_internal_heap(void)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "internal heap: free=%u allocated=%u min_free=%u largest=%u free_blocks=%u alloc_blocks=%u",
             (unsigned)info.total_free_bytes, (unsigned)info.total_allocated_bytes,
             (unsigned)info.minimum_free_bytes, (unsigned)info.largest_free_block, (unsigned)info.free_blocks,
             (unsigned)info.allocated_blocks);
    dram_watch_dump_t d = {.n = 0, .unlisted = 0};
    heap_caps_walk(MALLOC_CAP_INTERNAL, dump_walker, &d);
    for (unsigned i = 0; i < d.n; i++) {
        const dram_watch_region_t *r = &d.r[i];
        ESP_LOGW(TAG, "  region 0x%08x len %u: free=%u largest=%u free_blocks=%u used_blocks=%u",
                 (unsigned)r->start, (unsigned)(r->end - r->start), (unsigned)r->free_bytes,
                 (unsigned)r->largest_free, (unsigned)r->free_blocks, (unsigned)r->used_blocks);
    }
    if (d.unlisted) {
        ESP_LOGW(TAG, "  %u more region(s) not listed", d.unlisted);
    }
}

void dram_watch_service(void)
{
    dram_watch_pending_t p;
    portENTER_CRITICAL(&s_mux);
    p = dram_watch_take_pending(&s_w);
    portEXIT_CRITICAL(&s_mux);
    if (p.new_low) {
        ESP_LOGI(TAG, "internal largest free block new low %u B at uptime %u s", (unsigned)p.low_largest,
                 (unsigned)p.low_at_s);
    }
    if (p.alarm) {
        ESP_LOGW(TAG,
                 "internal largest free block %u B fell below the SK-04 alarm (%u B) at uptime %u s; "
                 "dumping internal heap once",
                 (unsigned)p.alarm_largest, (unsigned)KILN_DRAM_LARGEST_ALARM_BYTES, (unsigned)p.alarm_at_s);
        dump_internal_heap();
    }
}

void dram_watch_log_task(const char *task, const char *phase)
{
    ESP_LOGI(TAG, "%s %s largest=%u free=%u", task ? task : "?", phase ? phase : "?",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
