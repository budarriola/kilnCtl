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

// 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's final finding:
// this was configMINIMAL_STACK_SIZE (256 words / 1KB) unmultiplied --
// confirmed by the reset-surviving vApplicationStackOverflowHook() latch
// (main.c) as the actual overflowing task, after safety_core_task (fixed
// earlier the same day, *4 then *6) was cleared by the same mechanism.
//
// The arithmetic, not a guess: log_task_fn()'s own frame holds
// log_entry_t entry (2 + LOG_ENTRY_MSG_MAX(96) = 98 bytes) and
// uint8_t payload[1 + LOG_ENTRY_MSG_MAX] (97 bytes) live simultaneously in
// the branch that sends -- call it 195 bytes plus ~10 bytes of scalars
// (the float fill fraction, the BaseType_t receive result). That calls
// link_task_send_log() (a thin, near-zero-cost forward) into
// link_task_send_broadcast_to() (link_task.c), which is where the real
// cost is: kilnlink_frame_t frame (~16 bytes) plus
// uint8_t raw[KILNLINK_FRAME_RAW_MAX] -- confirmed 263 bytes
// (8 header + 253 max payload + 2 CRC, kilnlink_frame.h) -- plus
// uint8_t stuffed[KILNLINK_FRAME_STUFFED_MAX] -- confirmed 528 bytes
// (263 * 2 + 2, worst-case byte-stuffing) -- plus a few scalars. That is
// 263 + 528 = 791 bytes of buffers alone in ONE function's frame, the
// dominant cost by a wide margin; the coordinator's own estimate of
// "~530 and ~1060 bytes" was roughly double the real figures on both
// counts, corrected here against the actual #define values rather than
// repeated.
//
// Known, exact-by-construction total: 205 (log_task_fn) + ~16
// (link_task_send_log's own thin frame) + 811 (frame struct + raw[263] +
// stuffed[528] + status/stuffed_len scalars in link_task_send_broadcast_to)
// = ~1032 bytes, already exceeding the old 1024-byte budget on the known
// buffers alone, before counting a single byte of compiler-generated
// register-save/call-frame overhead across the ~4 nested calls in this
// chain (log_task_fn -> link_task_send_log -> link_task_send_broadcast_to
// -> kilnlink_frame_encode_raw/kilnlink_stuff) or the two leaf calls' own
// small frames. Call that overhead 100-150 bytes, unmeasured (this
// codebase's history: a per-checkin uxTaskGetStackHighWaterMark() census
// was tried and removed the same day for distorting watchdog timing,
// watchdog_task.c's own comment -- that was about instrumenting EVERY
// task's EVERY check-in, not a one-off measurement of this one path, but
// nothing here re-attempts even that lighter version, so treat the
// overhead figure as an estimate, not a measurement) -- call the true
// peak ~1130-1180 bytes.
//
// *2 (512 words / 2048 bytes) is the number this arithmetic supports: a
// stated ~1.7-1.8x margin over the ~1130-1180 byte estimate, deliberately
// smaller than link_task's own *6 (6144 bytes) -- link_task's worst case
// nests an RX unstuffed[~520] buffer AND a handler payload AND this same
// raw[263]/stuffed[528] pair simultaneously (that file's own comment), a
// genuinely deeper chain than log_task's single send path, so copying its
// multiplier here would not be reasoned, just imitated.
// test/test_log_task_stack_budget.c enforces the floor as a source-text
// check, same discipline as safety_core.c's own SAFETY_CORE_STACK_WORDS
// guard, so a future edit cannot silently shrink this back toward the
// value that just caused a live reboot.
#define LOG_TASK_STACK_WORDS   (configMINIMAL_STACK_SIZE * 2)
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
// status/diag send to find room.
//
// STALE justification, left uncorrected on purpose (2026-08-23, the
// DIAG-frame-went-dark investigation): the paragraph this replaced argued
// 50% was "comfortably conservative" against a link that "in practice uses
// roughly 5% of its 115200-baud budget on telemetry". uart_owner.c's baud
// rate was dropped 115200 -> 9600 on 2026-08-23 (a ~12x cut, TCMT1109
// optocoupler switching-speed limit, see that file's own comment) and
// nothing about this fraction was re-derived afterward -- so the number
// below predates the very budget it now has to operate under, and whether
// 0.5 is still the right value for a ~960 B/s link is presently unmeasured,
// not re-confirmed.
//
// A round-robin reordering of link_task_fn()'s status/diag/power sends was
// tried and reverted the same investigation day: it was built on the
// (plausible, but wrong) theory that program order was starving diag/power
// under TX-ring congestion. The tx_dropped_sat (link_frame.h's Frame A V2)
// and broadcast_dropped (KilnFW's uart_protocol.c) counters added alongside
// it both read a clean 0 on the exact hardware run where diag/power were
// still completely silent -- proving neither the Pico's TX ring nor the
// ESP's inbox was dropping anything, which rules out arbitration/congestion
// as the mechanism entirely. Whatever is actually happening to diag/power is
// upstream of both instruments (see link_task_send_diag()'s own history for
// the live investigation), not a reordering problem, so reordering was
// reverted rather than kept "just in case" against a premise that evidence
// had already killed. Those two counters are the real, load-bearing result
// of that pass and remain in place; do not treat their cleanliness as
// evidence this fraction is fine too -- it answers a different question.
// Changing 0.5 on a guess, without a real measurement of THIS fraction, is
// exactly what this comment is here to prevent.
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
