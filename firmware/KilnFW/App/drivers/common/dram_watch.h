/* Internal-DRAM largest-free-block low-water mark (SK-04 instrumentation).
 *
 * WHY: heap_caps_get_minimum_free_size() is a low-water mark of TOTAL free
 * bytes. SK-04 is about the LARGEST contiguous block (KILN_DRAM_LARGEST_
 * ALARM_BYTES, dram_margin.h), which fragmentation erodes while total free
 * barely moves, and the only existing reading of it was point-in-time (/api/
 * status, heap_stage at boot). A board read 8192 B after ~57000 s with no
 * way to see when or after what it dropped. This samples it from a cheap
 * esp_timer (no new task, no new stack) and remembers the lowest value and
 * the uptime second it was first seen at, so /api/status can report it.
 *
 * Also: dram_watch_log_task() prints one terse line (largest + total free,
 * internal) around each on-demand task creation, so a log shows which task
 * start moves the block; and the first time the sample falls below the
 * alarm figure the internal heap layout is dumped once, per region, as
 * ESP_LOGW lines (so it reaches the PC log link, which printf-based
 * heap_caps_print_heap_info() would bypass -- uart_log_bridge.c replaces the
 * console via esp_log_set_vprintf()). The esp_timer callback only samples
 * and records; every log line comes from dram_watch_service(), run by
 * telemetry_log_task(), so nothing heavy runs on the shared esp_timer task
 * (3584 B stack, CONFIG_ESP_TIMER_TASK_STACK_SIZE) or delays other timers.
 *
 * The tracker below is pure (host-tested in test_dram_watch.c); everything
 * that needs ESP-IDF is behind ESP_PLATFORM, with no-op stand-ins on the host
 * so call sites in host-compiled files need no stubs. */
#ifndef DRAM_WATCH_H
#define DRAM_WATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dram_margin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;           /* at least one sample recorded */
    size_t low_largest;   /* lowest largest-free-block seen, bytes */
    uint32_t low_at_s;    /* uptime second of the FIRST sample that read that value */
    bool alarm_reported;  /* the one-shot alarm dump has already been requested */
    /* Set by dram_watch_update(), cleared by dram_watch_take_pending(). The
     * sampler only records; the logging (and the one-shot heap dump) runs
     * later from a task, never inside the esp_timer callback -- see
     * dram_watch_service(). */
    bool new_low_pending;
    bool alarm_pending;
    size_t alarm_largest;  /* the sample that first fell below the alarm */
    uint32_t alarm_at_s;
} dram_watch_t;

typedef struct {
    bool new_low;      /* this sample set a new (strictly lower) low-water mark */
    bool alarm_first;  /* first sample ever below KILN_DRAM_LARGEST_ALARM_BYTES */
} dram_watch_event_t;

/* What dram_watch_service() has to log, taken atomically from the tracker. */
typedef struct {
    bool new_low;           /* at least one new low since the last take */
    size_t low_largest;     /* the CURRENT low (the latest of any coalesced new lows) */
    uint32_t low_at_s;
    bool alarm;             /* the one-shot alarm is owed */
    size_t alarm_largest;
    uint32_t alarm_at_s;
} dram_watch_pending_t;

static inline void dram_watch_init(dram_watch_t *w)
{
    w->valid = false;
    w->low_largest = 0;
    w->low_at_s = 0;
    w->alarm_reported = false;
    w->new_low_pending = false;
    w->alarm_pending = false;
    w->alarm_largest = 0;
    w->alarm_at_s = 0;
}

static inline dram_watch_event_t dram_watch_update(dram_watch_t *w, size_t largest, uint32_t uptime_s)
{
    dram_watch_event_t ev = {false, false};
    if (!w->valid || largest < w->low_largest) {
        w->valid = true;
        w->low_largest = largest;
        w->low_at_s = uptime_s;
        w->new_low_pending = true;
        ev.new_low = true;
    }
    if (largest < KILN_DRAM_LARGEST_ALARM_BYTES && !w->alarm_reported) {
        w->alarm_reported = true;
        w->alarm_pending = true;
        w->alarm_largest = largest;
        w->alarm_at_s = uptime_s;
        ev.alarm_first = true;
    }
    return ev;
}

/* Copies out and clears whatever is owed. Several new lows between two takes
 * coalesce into one (the current low), so the log line count is bounded by
 * the service rate, not the sample rate. The alarm is owed at most once per
 * boot (alarm_reported is never cleared). */
static inline dram_watch_pending_t dram_watch_take_pending(dram_watch_t *w)
{
    dram_watch_pending_t p;
    p.new_low = w->new_low_pending;
    p.low_largest = w->low_largest;
    p.low_at_s = w->low_at_s;
    p.alarm = w->alarm_pending;
    p.alarm_largest = w->alarm_largest;
    p.alarm_at_s = w->alarm_at_s;
    w->new_low_pending = false;
    w->alarm_pending = false;
    return p;
}

#if defined(ESP_PLATFORM)
/* Idempotent. Takes the first sample immediately, then every 2 s. */
void dram_watch_start(void);
/* false until the first sample exists. */
bool dram_watch_get(size_t *low_largest, uint32_t *low_at_s);
/* Logs whatever the sampler has flagged since the last call (a new low, and
 * once per boot the alarm plus a per-region internal heap summary). Call from
 * a task with ordinary ESP_LOG headroom; telemetry_log_task() calls it every
 * tick. Cheap when nothing is owed (one critical section). */
void dram_watch_service(void);
/* One INFO line: "dram_watch: <task> <phase> largest=N free=N". */
void dram_watch_log_task(const char *task, const char *phase);
/* Logs the "after" line and returns `created` unchanged, so a call site wraps the
 * xTaskCreate() expression in place: dram_watch_task_after("x", xTaskCreate(...)) != pdPASS. */
static inline int dram_watch_task_after(const char *task, int created)
{
    dram_watch_log_task(task, "after-create");
    return created;
}
#else
static inline void dram_watch_start(void) {}
static inline bool dram_watch_get(size_t *low_largest, uint32_t *low_at_s)
{
    (void)low_largest;
    (void)low_at_s;
    return false;
}
static inline void dram_watch_service(void) {}
static inline void dram_watch_log_task(const char *task, const char *phase)
{
    (void)task;
    (void)phase;
}
static inline int dram_watch_task_after(const char *task, int created)
{
    (void)task;
    return created;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* DRAM_WATCH_H */
