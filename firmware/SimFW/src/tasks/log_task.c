// log_task.c -- see log_task.h. The log queue (a bounded FreeRTOS queue,
// SMP-safe multi-writer/multi-core, same reasoning
// ../../SaftyFW/src/tasks/log_task.c's own header comment gives for using
// one instead of a hand-rolled single-producer ring), the drop counter, and
// the retained-entries ring a future query command reads. No wire send --
// see log_task.h's transport-decision comment for why.
#include "log_task.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "task_priorities.h"

#define LOG_TASK_STACK_WORDS configMINIMAL_STACK_SIZE

// Bounded wait on the queue receive below, not an indefinite block -- keeps
// this task's loop responsive to a level/consumer change even when the
// queue is idle, same reasoning SaftyFW's own LOG_TASK_POLL_MS carries
// (there tied to a watchdog checkin this project has none of yet).
#define LOG_TASK_POLL_MS 500u

// Queue depth: a burst of log lines faster than log_task can drain them
// fills this before anything is dropped. 16 matches SaftyFW's own choice --
// this is not expected to be a chatty system by default (LOG_LEVEL_WARN is
// the compiled-in default level).
#define LOG_TASK_QUEUE_LEN 16u

// One inbound queue entry -- fixed size, no pointers into caller memory
// (the caller's own buffer may not outlive log_task_log()'s return), the
// message is copied in once, here.
typedef struct {
    uint8_t level;
    uint8_t len; // bytes of msg actually used
    char msg[LOG_TASK_ENTRY_MSG_MAX];
} log_task_queue_entry_t;

static TaskHandle_t s_task_handle = NULL;
static QueueHandle_t s_log_queue = NULL;

static volatile uint8_t s_level = LOG_LEVEL_WARN;
static volatile uint32_t s_dropped = 0;

// Retained ring for log_task_get_entries() -- written only by log_task_fn()
// (this task, sole producer into the ring even though the inbound queue is
// multi-writer), read by any task under s_retain_lock. Mirrors
// i2c_owner.h's edge-log ring shape (fixed capacity, monotonic seq, oldest
// overwritten on wrap).
static log_task_entry_t s_retain[LOG_TASK_RETAIN_CAPACITY];
static uint32_t s_retain_next_write = 0; // index mod capacity
static uint32_t s_retain_next_seq = 1;   // next seq to assign; 0 is never used so
                                          // since_seq == 0 unambiguously means
                                          // "from the oldest retained entry"
static uint32_t s_retain_count = 0;      // number of valid entries so far (caps at capacity)
static SemaphoreHandle_t s_retain_lock = NULL;

// s_dropped is incremented from log_task_log() (any task, either core) AND
// could in principle be read concurrently -- wrapped in a critical section
// so the count is exact rather than "usually right", same pattern
// SaftyFW's log_task.c uses for its own cross-core counter.
static void log_task_count_dropped(void)
{
    taskENTER_CRITICAL();
    s_dropped++;
    taskEXIT_CRITICAL();
}

static void log_task_retain(const log_task_queue_entry_t *e)
{
    if (xSemaphoreTake(s_retain_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        // Should not happen (log_task is this mutex's only writer and never
        // holds it for more than a memcpy) -- if it ever does, the entry is
        // simply not retained; log_task_get_dropped() does not count this
        // (per log_task.h's own doc: retained-ring aging/misses are not
        // "dropped", they were logged and drained just fine).
        return;
    }

    log_task_entry_t *slot = &s_retain[s_retain_next_write];
    slot->seq = s_retain_next_seq++;
    slot->level = e->level;
    slot->len = e->len;
    memcpy(slot->msg, e->msg, e->len);

    s_retain_next_write = (s_retain_next_write + 1u) % LOG_TASK_RETAIN_CAPACITY;
    if (s_retain_count < LOG_TASK_RETAIN_CAPACITY) {
        s_retain_count++;
    }

    xSemaphoreGive(s_retain_lock);
}

static void log_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        log_task_queue_entry_t entry;
        BaseType_t got = xQueueReceive(s_log_queue, &entry, pdMS_TO_TICKS(LOG_TASK_POLL_MS));

        if (got == pdTRUE) {
            log_task_retain(&entry);
        }
    }
}

bool log_task_start(void)
{
    s_log_queue = xQueueCreate(LOG_TASK_QUEUE_LEN, sizeof(log_task_queue_entry_t));
    if (s_log_queue == NULL) {
        return false;
    }

    s_retain_lock = xSemaphoreCreateMutex();
    if (s_retain_lock == NULL) {
        return false;
    }

    s_dropped = 0;
    s_retain_next_write = 0;
    s_retain_next_seq = 1;
    s_retain_count = 0;
    memset(s_retain, 0, sizeof(s_retain));

    BaseType_t ok = xTaskCreate(log_task_fn, "log_task", LOG_TASK_STACK_WORDS, NULL,
                                 SIMFW_PRIO_LOG_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}

bool log_task_log(uint8_t level, const char *tag, const char *msg)
{
    if (s_log_queue == NULL) {
        return false; // log_task_start() never ran or failed
    }
    if (level > s_level) {
        return false; // filtered by the runtime level -- not a drop
    }

    log_task_queue_entry_t entry;
    entry.level = level;
    entry.len = 0;

    if (tag != NULL && tag[0] != '\0') {
        size_t room = LOG_TASK_ENTRY_MSG_MAX;
        size_t tag_len = 0;
        while (tag_len < room && tag[tag_len] != '\0') {
            tag_len++;
        }
        // Reserve room for ": " after the tag if it fits; truncate the tag
        // itself rather than overflow if it somehow doesn't (LOG_TASK_ENTRY_MSG_MAX
        // is generous for this project's actual tags).
        if (tag_len + 2u <= room) {
            memcpy(entry.msg, tag, tag_len);
            entry.msg[tag_len] = ':';
            entry.msg[tag_len + 1u] = ' ';
            entry.len = (uint8_t)(tag_len + 2u);
        } else {
            memcpy(entry.msg, tag, room);
            entry.len = (uint8_t)room;
        }
    }

    if (msg != NULL) {
        // Bounded copy, not strnlen()+memcpy -- strnlen is POSIX, not
        // standard C, and nothing else in this codebase relies on the
        // arm-none-eabi newlib build having it (same reasoning SaftyFW's
        // log_task.c gives for its own identical loop).
        size_t room = LOG_TASK_ENTRY_MSG_MAX - entry.len;
        size_t msg_len = 0;
        while (msg_len < room && msg[msg_len] != '\0') {
            msg_len++;
        }
        memcpy(&entry.msg[entry.len], msg, msg_len);
        entry.len = (uint8_t)(entry.len + msg_len);
    }

    if (xQueueSend(s_log_queue, &entry, 0) != pdTRUE) {
        log_task_count_dropped();
        return false;
    }
    return true;
}

void log_task_set_level(uint8_t level)
{
    s_level = level;
}

uint8_t log_task_get_level(void)
{
    return s_level;
}

uint32_t log_task_get_dropped(void)
{
    return s_dropped;
}

size_t log_task_get_entries(log_task_entry_t *out, size_t max_out, uint32_t since_seq)
{
    if (out == NULL || max_out == 0 || s_retain_lock == NULL) {
        return 0;
    }

    if (xSemaphoreTake(s_retain_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }

    // Oldest retained entry's seq, and the first seq strictly after
    // since_seq (this function's contract: "seq > since_seq").
    uint32_t oldest_seq = s_retain_next_seq - s_retain_count;
    uint32_t start_seq = since_seq + 1u;
    if (start_seq < oldest_seq) {
        start_seq = oldest_seq;
    }

    size_t n = 0;
    for (uint32_t seq = start_seq; seq < s_retain_next_seq && n < max_out; seq++) {
        uint32_t age = s_retain_next_seq - seq;      // 1 == most recently written
        uint32_t idx = (s_retain_next_write + LOG_TASK_RETAIN_CAPACITY - age) % LOG_TASK_RETAIN_CAPACITY;
        out[n] = s_retain[idx];
        n++;
    }

    xSemaphoreGive(s_retain_lock);
    return n;
}
