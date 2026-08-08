#include "uart_log_bridge.h"

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
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) {
        return n;
    }

    const char *text = line;
    size_t text_len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
    uint8_t level = uart_log_parse_level(&text, &text_len);

    if (s_bridge.queue && xTaskGetCurrentTaskHandle() != s_bridge.sender_task) {
        uart_log_entry_t entry;
        entry.level = level;
        size_t copy_len = text_len < sizeof(entry.text) ? text_len : sizeof(entry.text);
        memcpy(entry.text, text, copy_len);
        entry.len = (uint8_t)copy_len;
        xQueueSend(s_bridge.queue, &entry, 0);
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
    BaseType_t created = xTaskCreatePinnedToCore(uart_log_bridge_task, "uart_log_bridge", 4096,
                                                  &s_bridge, 4, &s_bridge.sender_task,
                                                  tskNO_AFFINITY);
    if (created != pdPASS) {
        s_bridge.proto = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
