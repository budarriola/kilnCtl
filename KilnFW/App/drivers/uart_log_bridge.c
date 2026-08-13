#include "uart_log_bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
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
 * uart_log_bridge_start() before keeping it. */
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
            s_dropped_lines++;
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
         * and best-effort is the whole point here. */
        uart_protocol_send(bridge->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_LOG,
                            UART_TASK_ID_LOG, payload, (size_t)entry.len + 1,
                            UART_LOG_BRIDGE_ACK_TIMEOUT_MS);

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
            uart_protocol_send(bridge->proto, UART_PROTO_DEVICE_HOST, UART_TASK_ID_LOG,
                                UART_TASK_ID_LOG, drop_payload, drop_len + 1,
                                UART_LOG_BRIDGE_ACK_TIMEOUT_MS);
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
    BaseType_t created = xTaskCreatePinnedToCore(uart_log_bridge_task, "uart_log_bridge", 4096,
                                                  &s_bridge, 7, &s_bridge.sender_task,
                                                  tskNO_AFFINITY);
    if (created != pdPASS) {
        s_bridge.proto = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
