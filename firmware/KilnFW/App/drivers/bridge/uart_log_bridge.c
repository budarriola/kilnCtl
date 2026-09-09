#include "uart_log_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "stack_margin.h"
#include "uart_task_ids.h"

/* How many formatted-but-not-yet-sent log lines we're willing to hold. Sized
 * generously (each entry is ~130 bytes) since this is what absorbs the
 * boot-time backlog (everything logged before uart_log_bridge_start() runs)
 * plus whatever accumulates if the PC is briefly not listening. */
/* NOTE (2026-08-12): 64 is measurably too small for the boot backlog -- every
 * reset on the bench reports "33 log line(s) dropped (queue full)" then "8
 * dropped", and the lines lost are exactly app_main's peripheral bring-up
 * results, i.e. the most diagnostic lines the board produces, gone precisely
 * when something failed to come up. Raising it to 192 (~25 KB at ~130 bytes
 * an entry) was tried and reverted the same session: the bench unit's PC
 * link stopped answering afterward and there was no way to tell whether the
 * bump caused it (the link is also the only way to observe the log). Retry
 * this with a working link, and confirm free heap after
 * uart_log_bridge_start() before keeping it.
 * NOTE (2026-08-19): retried at 256 -- board hung at the "kilnCtl Ready"
 * splash, never reaching the home UI. Reverted same session. This CONFIRMS
 * the 2026-08-12 note's suspicion: this queue's size is a real boot-hang
 * trigger, not a red herring. Do not raise this again without figuring out
 * why first (heap exhaustion vs. something in xQueueCreate's allocation
 * path) -- the PC-side buffer (kilnctrl/mcp_server.py, byte-budgeted to
 * 1 MiB) is the right place to hold more history; this queue must stay
 * exactly big enough to survive the boot burst and no bigger. */
#define UART_LOG_BRIDGE_QUEUE_LEN 64

/* One payload's worth of text: UART_PROTO_MAX_PAYLOAD minus the level byte. */
#define UART_LOG_TEXT_MAX (UART_PROTO_MAX_PAYLOAD - 1)

typedef struct {
    uint8_t level;
    uint8_t len;
    char text[UART_LOG_TEXT_MAX];
} uart_log_entry_t;

/* Per-attempt ACK timeout for the log task's own sends -- long enough to
 * cover a normal round trip through a host driver (the default across the
 * rest of the protocol; see UART_PROTO_DEFAULT_ACK_TIMEOUT_MS) without
 * routinely timing out and forcing a retry under ordinary jitter. Still
 * short relative to how long a fully disconnected/backed-up link would
 * otherwise stall delivery for, given uart_protocol_send's own internal
 * UART_PROTO_MAX_RETRIES retries on top of this. */
#define UART_LOG_BRIDGE_ACK_TIMEOUT_MS UART_PROTO_DEFAULT_ACK_TIMEOUT_MS

/* 2026-08-20 congestion fix (see ROADMAP.md/TODO.md): this task used to call
 * plain uart_protocol_send(), which retries UART_PROTO_MAX_RETRIES (10)
 * times against uart_protocol.h's default ack_timeout_ms before giving up --
 * despite this call site's own comment two paragraphs down already saying
 * "Fire-and-forget: the result is intentionally ignored". That mismatch was
 * the actual mechanism behind the bench-observed PC-link congestion: with no
 * RP2040 attached, safety_link.c's poll retries (dev2/task7) throw a
 * rate-limited "no reply" WARNing roughly every 5s (uart_protocol.c's
 * RETRY_LOG_INTERVAL_US) -- and *that* line, like every ESP_LOGx call, gets
 * captured by uart_log_vprintf() and forwarded here as an ordinary DATA
 * frame on the *same* proto (and therefore the same tx_lock) uart_bridge.c
 * uses for real PC replies. If the PC side is even briefly slow to ACK, one
 * such forward could hold that shared tx_lock for up to
 * UART_LOG_BRIDGE_ACK_TIMEOUT_MS * UART_PROTO_MAX_RETRIES (2000ms) --
 * directly explaining the observed 2-3s+ reply delays and the "PC link lost
 * (no frame or ACK for 5000ms)" events under heavier load, since every
 * *other* queued reply serializes on that same lock behind it.
 *
 * A true BROADCAST send (uart_protocol_send_broadcast, zero retries, returns
 * as soon as the bytes are queued) would remove the cost entirely, but the
 * PC-side reader only delivers MsgType.DATA frames to a registered task's
 * inbox (tools/PcTools/src/kilnctrl/serial_link.py's _handle_frame():
 * anything that "is not MsgType.DATA" is silently dropped) -- switching this
 * channel's frame type would make device logging go dark on the PC side, not
 * just cheaper. uart_protocol_send_limited() keeps the DATA frame (so
 * delivery still works) while capping the retry budget to what this call
 * site already claimed it wanted: try once, keep going either way. */
#define UART_LOG_BRIDGE_MAX_RETRIES 1

typedef struct {
    QueueHandle_t queue;
    uart_protocol_t *proto;      /* NULL until uart_log_bridge_start() */
    TaskHandle_t sender_task;    /* set once uart_log_bridge_task() starts */
} uart_log_bridge_t;

/* Single instance -- there is exactly one log stream for the whole firmware,
 * same pattern as the other uart_bridge.c task contexts (static, lives for
 * the program's duration). */
static uart_log_bridge_t s_bridge;

/* Lines lost to a full queue (producer outrunning the sender, e.g. a burst
 * right before a reset) since the last time this was reported. Not a
 * critical-section-protected counter -- an occasional lost increment under
 * concurrent ESP_LOGx callers just means the reported count is a lower
 * bound, which is still far better than the previous silent-drop behavior. */
static volatile uint32_t s_dropped_lines = 0;

/* ESP-IDF's default formatted line looks like (optionally ANSI-colored):
 *   "E (12345) TAG: message\r\n"
 * Strip a leading color escape (if present), pull the level off the first
 * character, and strip the trailing color reset / CR / LF -- keeping the
 * rest verbatim so TAG/timestamp/message all still show up on the PC side. */
static uint8_t uart_log_parse_level(const char **text, size_t *len)
{
    const char *p = *text;
    size_t n = *len;

    if (n >= 2 && p[0] == '\033') {
        const void *m = memchr(p, 'm', n);
        if (m) {
            size_t skip = (size_t)((const char *)m - p) + 1;
            p += skip;
            n -= skip;
        }
    }

    uint8_t level = UART_LOG_LEVEL_INFO;
    if (n >= 1) {
        switch (p[0]) {
            case 'E': level = UART_LOG_LEVEL_ERROR; break;
            case 'W': level = UART_LOG_LEVEL_WARN; break;
            case 'I': level = UART_LOG_LEVEL_INFO; break;
            case 'D': level = UART_LOG_LEVEL_DEBUG; break;
            case 'V': level = UART_LOG_LEVEL_VERBOSE; break;
            default: break;
        }
    }

    while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) {
        n--;
    }
    if (n >= 4 && memcmp(p + n - 4, "\033[0m", 4) == 0) {
        n -= 4;
    }

    *text = p;
    *len = n;
    return level;
}

/* Installed via esp_log_set_vprintf(): replaces console logging entirely
 * (not in addition to it -- see uart_log_bridge.h) with a non-blocking push
 * into the queue uart_log_bridge_task() drains. Formatting a line and
 * enqueueing it is fast and allocation-free, so this is safe to call from
 * any task context that can legally call ESP_LOGx today; xQueueSend's 0
 * timeout means a full queue (or one that doesn't exist yet, e.g. creation
 * failed) just drops the line instead of ever blocking the caller.
 *
 * Lines logged from uart_log_bridge_task() itself (e.g. uart_protocol.c's
 * own "no reply, retry N/M" warning when *its* send needs a retry) are
 * dropped here rather than re-enqueued: forwarding "failed to forward a log
 * line" back through the same channel it's complaining about is not useful
 * to a reader, and every one queued is one more send that can itself need a
 * retry -- a self-inflicted multiplier on exactly the kind of link trouble
 * that produced the warning in the first place. */
static int uart_log_vprintf(const char *fmt, va_list args)
{
    char line[160];
#if CONFIG_KILNCTL_LOG_TEE_CONSOLE
    /* Tee to the console UART as well as the PC link. The queue below drops
     * lines when full, and it reliably IS full during the boot burst -- which
     * is exactly when the interesting lines (peripheral bring-up results,
     * httpd registration, Wi-Fi state) are produced. Without this, a dropped
     * boot line is gone for good and the board's own account of why it came
     * up wrong is unrecoverable. va_copy because args is consumed by the
     * first vprintf. */
    {
        va_list console_args;
        va_copy(console_args, args);
        vprintf(fmt, console_args);
        va_end(console_args);
    }
#endif
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) {
        return n;
    }

    /* vsnprintf truncates silently at the buffer boundary; note it here so a
     * long line reads as "...[+N]" on the PC side instead of just quietly
     * ending mid-word with no sign anything was cut. */
    bool line_truncated = (size_t)n >= sizeof(line);
    size_t overflow = line_truncated ? (size_t)n - (sizeof(line) - 1) : 0;

    const char *text = line;
    size_t text_len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
    uint8_t level = uart_log_parse_level(&text, &text_len);

    if (s_bridge.queue && xTaskGetCurrentTaskHandle() != s_bridge.sender_task) {
        uart_log_entry_t entry;
        entry.level = level;
        /* Reserve room for a "...[+N]" truncation marker so the copy below
         * never fights the vsnprintf truncation for the same bytes. */
        char marker[16];
        size_t marker_len = 0;
        if (line_truncated) {
            int m = snprintf(marker, sizeof(marker), "...[+%u]", (unsigned)overflow);
            marker_len = (m > 0 && (size_t)m < sizeof(marker)) ? (size_t)m : 0;
        }
        size_t max_text = sizeof(entry.text) - marker_len;
        size_t copy_len = text_len < max_text ? text_len : max_text;
        memcpy(entry.text, text, copy_len);
        memcpy(entry.text + copy_len, marker, marker_len);
        entry.len = (uint8_t)(copy_len + marker_len);
        if (xQueueSend(s_bridge.queue, &entry, 0) != pdTRUE) {
            /* An ERROR or WARN line is the kind this queue must not lose.
             * The 2026-08-12 note above says it exactly for ERROR: the lines
             * dropped during the boot burst "are exactly app_main's
             * peripheral bring-up results ... gone precisely when something
             * failed to come up". 2026-09-07 audit (log_eviction_2026-09-07.md):
             * the same class hits WARN too -- this codebase logs plenty of
             * genuine failures at WARN, not just ERROR (persist/NVS
             * failures, safety-link loss, config-decode refusals like
             * ZONES_DECODE_NEWER, task-start failures), and the eviction
             * policy used to protect ERROR only. Under boot-burst or
             * sustained-load pressure those WARN failures were dropped with
             * no eviction attempted at all -- silently indistinguishable
             * from never having been logged. Rather than hunt down and
             * promote each such WARN to ERROR one at a time (a losing game
             * against every WARN added later), the eviction policy itself
             * now protects both severities: this is a level-based
             * "important" flag, not a per-callsite fix. Widening protection
             * to WARN does not risk an ERROR flood -- promotion of
             * individual WARNs to ERROR is unnecessary and undesirable now.
             *
             * Growing the queue is NOT the fix and must not be tried again
             * -- see the two reverted attempts above (192, then 256: boot
             * hang). Instead, buy room for an error/warning by evicting the
             * OLDEST queued line, which during a boot burst is almost
             * always routine INFO progress. The evicted line is still
             * counted as dropped, so the PC-side total stays honest; what
             * changes is only WHICH line survives when the queue is full,
             * and an error or warning outranks whatever routine line was
             * queued before it.
             *
             * Deliberately one eviction, not a loop: a storm of
             * errors/warnings must not be able to spin here draining the
             * whole queue, and the level filter means this path is rare by
             * construction.
             *
             * 2026-09-07 (log_eviction_2026-09-07.md, point 3): with WARN
             * now protected too, a sustained WARN storm can fill the queue
             * with nothing but protected entries. The single-eviction rule
             * above still bounds the cost, but picking the OLDEST entry
             * unconditionally meant an incoming WARN could evict a
             * genuinely rarer, higher-severity ERROR that happened to be at
             * the front -- ERROR and WARN were being treated as equally
             * disposable once both were "protected", which quietly
             * undoes the severity ordering (ERROR = definite failure,
             * WARN = degraded/recoverable) the rest of this codebase relies
             * on. Fix: an incoming ERROR may still evict anything (as
             * before); an incoming WARN may evict anything EXCEPT an ERROR.
             * This still requires draining the queue to find an eligible
             * victim and reinsert the rest in original order -- bounded by
             * the queue length (64), and only reached on this already-rare
             * full-and-protected path. If every queued entry is ERROR, an
             * incoming WARN finds no eligible victim and is dropped outright
             * rather than displacing an ERROR -- counted the same as any
             * other drop. */
            bool kept = false;
            if (level == UART_LOG_LEVEL_ERROR || level == UART_LOG_LEVEL_WARN) {
                UBaseType_t n = uxQueueMessagesWaiting(s_bridge.queue);
                bool victim_found = false;
                for (UBaseType_t i = 0; i < n; i++) {
                    uart_log_entry_t tmp;
                    if (xQueueReceive(s_bridge.queue, &tmp, 0) != pdTRUE) {
                        break; /* shouldn't happen -- single producer/consumer of this scan */
                    }
                    bool may_evict = (level == UART_LOG_LEVEL_ERROR) || (tmp.level != UART_LOG_LEVEL_ERROR);
                    if (!victim_found && may_evict) {
                        victim_found = true;
                        s_dropped_lines++; /* tmp is the evicted line, not requeued */
                        continue;
                    }
                    xQueueSend(s_bridge.queue, &tmp, 0); /* not the victim -- put back, order preserved */
                }
                if (victim_found) {
                    kept = (xQueueSend(s_bridge.queue, &entry, 0) == pdTRUE);
                }
            }
            if (!kept) {
                s_dropped_lines++;
            }
        }
    }

    return n;
}

static void uart_log_bridge_task(void *arg)
{
    uart_log_bridge_t *bridge = (uart_log_bridge_t *)arg;
    uart_log_entry_t entry;

    while (true) {
        if (xQueueReceive(bridge->queue, &entry, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint8_t payload[1 + UART_LOG_TEXT_MAX];
        payload[0] = entry.level;
        memcpy(&payload[1], entry.text, entry.len);

        /* Fire-and-forget: the result is intentionally ignored -- there is
         * nowhere to report it that wouldn't itself just be another log line
         * queued behind this one (see uart_log_vprintf's self-filter above),
         * and best-effort is the whole point here. _limited() with
         * UART_LOG_BRIDGE_MAX_RETRIES=1 makes that true in practice, not just
         * in this comment -- see that macro's definition for why the old
         * 10-retry uart_protocol_send() call here was the actual congestion
         * mechanism, not merely a slow path. */
        uart_protocol_send_limited(bridge->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_LOG,
                                    UART_TASK_ID_LOG, payload, (size_t)entry.len + 1,
                                    UART_LOG_BRIDGE_ACK_TIMEOUT_MS, UART_LOG_BRIDGE_MAX_RETRIES);

        /* Surface any lines lost to a full queue since the last report --
         * built directly as a payload (not through ESP_LOGx, which
         * uart_log_vprintf's self-filter would drop anyway since this is
         * running on sender_task) so a burst that outran the queue is
         * visible on the PC side as a specific count, not just a gap in the
         * transcript. Checked after every send, not just when the queue runs
         * dry, so a steady stream of overflow still gets reported promptly
         * instead of only once the device finally quiets down. */
        uint32_t dropped = s_dropped_lines;
        if (dropped > 0) {
            s_dropped_lines -= dropped;
            uint8_t drop_payload[1 + UART_LOG_TEXT_MAX];
            drop_payload[0] = UART_LOG_LEVEL_WARN;
            int m = snprintf((char *)&drop_payload[1], UART_LOG_TEXT_MAX,
                              "uart_log_bridge: %u log line(s) dropped (queue full)",
                              (unsigned)dropped);
            size_t drop_len = (m > 0 && (size_t)m < UART_LOG_TEXT_MAX) ? (size_t)m : UART_LOG_TEXT_MAX - 1;
            uart_protocol_send_limited(bridge->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_LOG,
                                        UART_TASK_ID_LOG, drop_payload, drop_len + 1,
                                        UART_LOG_BRIDGE_ACK_TIMEOUT_MS, UART_LOG_BRIDGE_MAX_RETRIES);
        }
    }
}

void uart_log_bridge_early_init(void)
{
    memset(&s_bridge, 0, sizeof(s_bridge));
    s_bridge.queue = xQueueCreate(UART_LOG_BRIDGE_QUEUE_LEN, sizeof(uart_log_entry_t));
    esp_log_set_vprintf(uart_log_vprintf);
}

esp_err_t uart_log_bridge_start(uart_protocol_t *proto)
{
    if (!proto || !s_bridge.queue) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bridge.proto = proto;
    /* Priority 7: above every other uart_bridge.c task (all priority 5) and
     * uart_owner/uart_protocol (5/6, see settings.h/Kconfig defaults) --
     * deliberately the highest-priority task in the app. Log delivery is
     * exactly what needs to keep running when something else is busy or
     * wedged (an I2C lockup, a burst of bridge traffic); at the old
     * priority 4 it was *below* the very tasks it reports on, so it could be
     * starved right when its output mattered most. */
    /* 2026-08-22: PSRAM stack. This task only formats and forwards log
     * lines over the existing uart_protocol inbox/queue plumbing -- no
     * flash/NVS access, no direct hardware ownership. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(uart_log_bridge_task, "uart_log_bridge", 4096,
                                                         &s_bridge, 7, &s_bridge.sender_task, tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_bridge.proto = NULL;
        return ESP_ERR_NO_MEM;
    }
    stack_margin_register("uart_log_bridge", &s_bridge.sender_task, 4096);
    return ESP_OK;
}
