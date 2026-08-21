// log_task.c -- Phase 8: the log ring (a bounded FreeRTOS queue, not a hand-
// rolled ring buffer -- see the queue's doc comment below for why), the
// TX-reserve watermark that protects telemetry (docs/ARCHITECTURE.md section
// 1, "a log frame must never be able to displace a telemetry frame"), and
// the dropped-frame counter. log_task itself has no isolation restriction
// (tools/check_isolation.ps1 only greps link_task.c/safety_core.c) -- it may
// reference link_task.h freely, which is exactly how it reaches the link:
// through link_task_send_log(), never by touching uart_owner directly.
#include "log_task.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "task_priorities.h"
#include "watchdog_task.h"

#include "console_uart.h"
#include "link_task.h"
#include "tx_watermark.h"

#define LOG_TASK_STACK_WORDS   configMINIMAL_STACK_SIZE
// Bounded wait on the queue receive below, not an indefinite block: this
// task still owns its own watchdog checkin, and a bounded wait is what lets
// it happen even when the queue is empty (mirrors link_task's own reasoning
// for why RX polling is bounded rather than blocking, link_task.c:52-54).
#define LOG_TASK_POLL_MS       500

// "TAG: message" bytes per entry. LINK_FRAME payload budget is generous
// (KILNLINK_FRAME_RAW_MAX, well over 128), but there is no reason for a
// single log line to approach it -- 96 bytes is ample for this project's
// actual log lines and keeps the queue's static footprint small
// (LOG_QUEUE_LEN * sizeof(log_entry_t), allocated once at
// log_task_start() -- see "no dynamic allocation after init", TODO.md
// "Deliberately not doing").
#define LOG_ENTRY_MSG_MAX      96u
// Queue depth: a burst of log lines faster than log_task can drain them
// (500ms-ish cadence below, though draining is really event-driven via
// xQueueReceive's own wait) fills this before anything is dropped. 16 is
// generous for a system that is not supposed to be chatty by default
// (LOG_LEVEL_WARN is the compiled-in default level).
#define LOG_QUEUE_LEN          16u

// One entry. Fixed size, no pointers into caller memory (the caller's
// buffer may not outlive the log_task_log() call) -- the message is copied
// in, once, here.
typedef struct {
    uint8_t level;
    uint8_t len; // bytes of msg actually used
    char    msg[LOG_ENTRY_MSG_MAX];
} log_entry_t;

static TaskHandle_t s_task_handle = NULL;

// A FreeRTOS queue, not a raw ring buffer: log_task_log() is called from
// ANY task, on either core (docs/ARCHITECTURE.md section 4's core split
// means a guard on SAFTYFW_CORE_TRIP_PATH can log too), so the producer side
// is genuinely multi-writer, multi-core. A hand-rolled single-producer/
// single-consumer ring (uart_owner's own pattern, whose header comment is
// explicit that its critical-section trick relies on both ends living on
// core 0) would need its own cross-core lock reinvented for no benefit --
// FreeRTOS's queue already is that lock, SMP-safe, and xQueueSend(...,0) is
// exactly as non-blocking as the raw-ring alternative would have been.
// Allocated once, at log_task_start(), matching relay_owner.c's own
// xQueueCreate() pattern (relay_owner.c) -- "no dynamic allocation after
// init" is about avoiding allocation on the hot path, not about avoiding
// xQueueCreate() at startup, which every command-queue task in this build
// already does.
static QueueHandle_t s_log_queue = NULL;

static volatile uint8_t s_level = LOG_LEVEL_WARN; // ARCHITECTURE.md section 1's default
static volatile uint32_t s_dropped = 0;

// s_dropped is incremented from log_task_log() (any task, either core) AND
// from log_task_fn() (this task, core 0) -- a genuinely cross-core,
// multi-writer counter, unlike uart_owner's s_tx_dropped (single core, IRQ +
// one task only, per uart_owner.c's own header comment). A bare `s_dropped++`
// is a non-atomic read-modify-write and could lose an increment under
// concurrent access from both cores; wrapping it in the same
// taskENTER_CRITICAL()/taskEXIT_CRITICAL() pattern current_task.c already
// uses for its own cross-task published state makes the count exact rather
// than "usually right".
static void log_task_count_dropped(void)
{
    taskENTER_CRITICAL();
    s_dropped++;
    taskEXIT_CRITICAL();
}

// TX-reserve watermark: log frames may only use TX ring space that remains
// after this fraction, reserving the rest for Frame A/B telemetry
// (docs/ARCHITECTURE.md section 1's two non-optional requirements). 0.5 (50%)
// is the starting number this pass picks: Frame A is 500ms-cadence and
// small (well under uart_owner's 128-byte ring per frame, uart_owner.c's own
// sizing comment), so even a burst of several queued log lines competing
// for the remaining half still leaves comfortable headroom for the next
// status/diag send to find room. A tighter reserve (e.g. 25%) would starve
// log throughput further for a link that in practice uses roughly 5% of its
// 115200-baud budget on telemetry (ARCHITECTURE.md section 1) -- there is no
// bandwidth pressure here, only the displacement risk the doc warns about,
// and 50% is a comfortably conservative answer to that without being
// needlessly stingy on debug visibility.
#define LOG_TX_RESERVE_FRACTION 0.5f

#ifdef SAFTYFW_ENABLE_USB_STDIO
// Bench-only mirror to stdio_usb, gated fully at compile time (CMakeLists.txt
// option, default OFF). docs/ARCHITECTURE.md section 1 is explicit this is
// "usable only with 12v_Safty removed" (HARDWARE.md section 7: A1's 3V3 is
// back-fed) and costs real things even when unused hardware-wise (TinyUSB
// linked into the safety build, a blocking-by-default printf risk) -- this
// firmware does not attempt to solve those tradeoffs, it only gates the code
// so a normal build never pays for or risks any of it.
static void log_task_mirror_usb(const log_entry_t *e)
{
    // fwrite, not printf/puts: the message is not null-terminated in the
    // entry (matches the wire format's own "not null-terminated" rule) and
    // may contain exactly len bytes, no more.
    fwrite(e->msg, 1, e->len, stdout);
    fputc('\n', stdout);
}
#endif

// Console mirror to UART0 (GP16/GP17, the debug probe's UART bridge) --
// console_uart.h's header comment explains why this is unconditional rather
// than gated like the USB mirror above: those pins carry nothing else on
// this board, so there is no back-feed/TinyUSB tradeoff to gate. This is the
// only place log entries reach the console; nothing else in this firmware
// should printf/puts directly (docs/ARCHITECTURE.md section 1's three-
// transport list -- this is the "RTT/UART bridge" secondary path's plain-
// UART leg).
static void log_task_mirror_console(const log_entry_t *e)
{
    console_uart_write(e->msg, e->len);
    console_uart_puts("\r\n");
}

static void log_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        log_entry_t entry;
        BaseType_t got = xQueueReceive(s_log_queue, &entry, pdMS_TO_TICKS(LOG_TASK_POLL_MS));

        if (got == pdTRUE) {
#ifdef SAFTYFW_ENABLE_USB_STDIO
            log_task_mirror_usb(&entry);
#endif
            log_task_mirror_console(&entry);
            // TX-reserve watermark (docs/ARCHITECTURE.md section 1, rule 1):
            // check BEFORE ever touching link_task/uart_owner, so a full-ish
            // ring loses log lines here, never telemetry frames -- Frame A/B
            // sends (link_task.c's own TX loop) never consult this at all,
            // by design, so they are never the ones that get held back.
            float fill = link_task_get_tx_ring_fill_fraction();
            if (tx_watermark_should_drop_log(fill, LOG_TX_RESERVE_FRACTION)) {
                log_task_count_dropped();
            } else {
                uint8_t payload[1 + LOG_ENTRY_MSG_MAX];
                payload[0] = entry.level;
                memcpy(&payload[1], entry.msg, entry.len);
                if (!link_task_send_log(payload, (uint8_t)(1u + entry.len))) {
                    log_task_count_dropped(); // rule 2: count it (uart_owner's own
                                               // TX-ring-full path already refused
                                               // the frame -- this is the "quiet
                                               // log vs nothing logged" distinction)
                }
            }
        }

        watchdog_task_checkin(WATCHDOG_CHECKIN_LOG_TASK);
    }
}

bool log_task_start(void)
{
    s_log_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_entry_t));
    if (s_log_queue == NULL) {
        return false;
    }
    s_dropped = 0;

    BaseType_t ok = xTaskCreate(log_task_fn, "log_task", LOG_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_LOG_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_LINK_PATH);
    return true;
}

bool log_task_log(uint8_t level, const char *tag, const char *msg)
{
    if (s_log_queue == NULL) {
        return false; // log_task_start() never ran or failed -- nothing to enqueue into
    }
    if (level > s_level) {
        return false; // filtered by the runtime level -- not a drop, see log_task.h
    }

    log_entry_t entry;
    entry.level = level;
    entry.len = 0;

    if (tag != NULL && tag[0] != '\0') {
        int n = snprintf(entry.msg, LOG_ENTRY_MSG_MAX, "%s: ", tag);
        if (n > 0) {
            entry.len = (uint8_t)((size_t)n < LOG_ENTRY_MSG_MAX ? (size_t)n
                                                                  : LOG_ENTRY_MSG_MAX - 1u);
        }
    }
    if (msg != NULL) {
        // Bounded copy, not strnlen()+memcpy: strnlen is POSIX, not standard
        // C, and nothing else in this codebase relies on the arm-none-eabi
        // newlib build having it -- this loop needs no such assumption.
        size_t room = LOG_ENTRY_MSG_MAX - entry.len;
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
