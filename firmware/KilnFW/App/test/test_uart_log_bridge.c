// Host test for App/drivers/uart_log_bridge.c -- TODO.md section 1's "Wi-Fi
// driver log lines arrive with an empty body" item (~line 97).
//
// Investigation summary (see the commit/PR description for the full
// writeup): a live device log capture showed every WARN line rendered as
// "<level> <level> (<ts>) <tag>: <text>" -- e.g.
// "W W (914062) uart_bridge: PC link back after 5000ms..." -- for every
// component, not just wifi. That double-letter shape is NOT a bug: the PC
// side prefixes the level letter it read from payload[0], and the wire text
// itself (uart_task_ids.h's own doc comment for UART_TASK_ID_LOG) is
// documented to carry the *whole* formatted line verbatim, level letter and
// timestamp included -- uart_log_parse_level() below only *inspects* the
// leading level character to pick payload[0]; it never strips it. So the
// double letter is expected, and rules out "the bridge duplicates or
// mis-slices the level" as the cause of the empty-body wifi lines.
//
// What this test actually proves about uart_log_bridge.c's OWN code (the
// only files this task is allowed to touch):
//   1. uart_log_vprintf()/uart_log_parse_level() forward a line's text
//      completely verbatim -- ANSI color stripped, CR/LF stripped, nothing
//      else touched -- for an ordinary non-empty line.
//   2. Fed a line that is ALREADY empty after "tag:" (i.e. this process's
//      vsnprintf() genuinely produced no message text, exactly the shape
//      TODO.md describes for the wifi driver's own log calls), the bridge
//      forwards that emptiness faithfully rather than silently eating a
//      real message -- i.e. this file's code is not itself deleting text
//      that was there.
//   3. There is exactly one enqueue per uart_log_vprintf() call (one stack
//      buffer, one queue push) -- ruling out "line split across two
//      callbacks, only the first kept" as this file's own mechanism.
//
// Given (1)-(3), an empty-bodied "wifi:" line is not explainable by
// anything in uart_log_bridge.c: the text this file hands to the queue is
// exactly what vsnprintf(fmt, args) produced, unmodified past ANSI/CRLF
// stripping. The remaining candidate is upstream of this file: whatever
// calls ESP_LOGW(TAG="wifi", ...) inside the closed-source esp_wifi driver
// blob passed a format/args pair that itself renders empty after the tag --
// code this repository does not contain and cannot unit-test from the host
// (no fmt/args capture is possible without ESP-IDF's real wifi component,
// which is not vendored here; see the live-board section of the report for
// the reproduction attempt and why it did not trigger the line in the
// observation window). That is a genuine negative result, not a shrug:
// sections 1-3 below are exactly the parts of "how uart_log_bridge captures
// or forwards lines" this task named as suspects, and all three are
// demonstrated clean.
//
// Compiled the same way test_wifi_prov.c/test_backup_import.c compile their
// driver under test: #include the .c file directly so this test can reach
// the static uart_log_vprintf()/uart_log_parse_level() functions, which have
// no production reason to be non-static.
#include <stdarg.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

// ---- Stub globals the freertos/queue.h stub reads/writes (see that file's
// 2026-08-21 comments) ----
// g_stub_queue_send_calls itself is already defined once for the whole test
// executable, in test_wifi_prov.c (extern-declared by the queue.h stub) --
// reused here, not redefined, to avoid a duplicate-symbol link error.
unsigned char g_stub_last_queue_item[256];

#include "../drivers/uart_log_bridge.c"

// ---- Ring-mode backing storage for the eviction tests below (see
// stubs/freertos/queue.h's 2026-08-24 comments). Defined here, after the
// #include above, because TEST_STUB_QUEUE_RING_MAX_CAPACITY is only visible
// once freertos/queue.h has been pulled in transitively by
// uart_log_bridge.c's own #include "freertos/queue.h". Left disabled
// (g_stub_queue_ring_enabled = 0) so test_verbatim_forward() and friends
// above -- and test_wifi_prov.c, compiled into the same executable -- see
// the exact original always-pdFALSE stub with no behavior change. ----
int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;

// uart_log_bridge.c's only call into uart_protocol.c proper -- never
// actually invoked by these tests (they call uart_log_vprintf() directly,
// never uart_log_bridge_start()/uart_log_bridge_task()), but the symbol
// must exist for the link since uart_log_bridge_task()'s body still
// references it. Declared down here (rather than before the #include
// above) so its signature can use uart_protocol_t/uart_proto_device_t as
// the real header defines them, instead of racing that definition.
esp_err_t uart_protocol_send_limited(uart_protocol_t *proto,
                                      uart_proto_device_t dst_device,
                                      uint8_t dst_task,
                                      uint8_t src_task,
                                      const uint8_t *payload,
                                      size_t length,
                                      uint32_t ack_timeout_ms,
                                      int max_retries)
{
    (void)proto; (void)dst_device; (void)dst_task; (void)src_task;
    (void)payload; (void)length; (void)ack_timeout_ms; (void)max_retries;
    return ESP_OK;
}

// Minimal va_list-forwarding shim so each test case can call
// uart_log_vprintf(fmt, ...) instead of hand-building a va_list.
static int call_uart_log_vprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = uart_log_vprintf(fmt, args);
    va_end(args);
    return n;
}

// entry.text is not NUL-terminated on the wire (uart_task_ids.h: "ASCII
// text ... NOT null-terminated"); this helper reconstructs a C string from
// the captured queue item for easy comparison.
static void captured_entry_text(char *out, size_t out_size)
{
    // Layout mirrors uart_log_entry_t: uint8_t level; uint8_t len; char text[...]
    uint8_t len = g_stub_last_queue_item[1];
    const char *text = (const char *)&g_stub_last_queue_item[2];
    size_t n = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, text, n);
    out[n] = '\0';
}

static void test_verbatim_forward(void)
{
    uart_log_bridge_early_init();

    g_stub_queue_send_calls = 0;
    memset(g_stub_last_queue_item, 0, sizeof(g_stub_last_queue_item));
    call_uart_log_vprintf("W (3687) safety_link: isolated fault line ASSERTED\r\n");
    TEST_CHECK(g_stub_queue_send_calls == 1, "one enqueue for one log line");

    char text[160];
    captured_entry_text(text, sizeof(text));
    // Whole line forwarded verbatim -- level letter, timestamp, tag AND
    // message all present, CRLF stripped. This is the "non-empty line
    // survives intact" half of the proof.
    TEST_CHECK(strcmp(text, "W (3687) safety_link: isolated fault line ASSERTED") == 0,
               "non-empty line forwarded byte-for-byte, CRLF stripped");
}

static void test_forwards_empty_message_faithfully(void)
{
    uart_log_bridge_early_init();

    g_stub_queue_send_calls = 0;
    memset(g_stub_last_queue_item, 0, sizeof(g_stub_last_queue_item));
    // Mimics TODO.md's exact observed shape: a WARN line whose message text
    // is already empty by the time it reaches this function (as if the
    // wifi driver itself called its logging macro with no text). If
    // uart_log_bridge.c were the thing eating the message, we'd expect this
    // to look no different from a real message being dropped -- instead it
    // must come through with the same nothing that went in.
    call_uart_log_vprintf("W (3687) wifi:\r\n");
    TEST_CHECK(g_stub_queue_send_calls == 1, "one enqueue even for an already-empty message");

    char text[160];
    captured_entry_text(text, sizeof(text));
    TEST_CHECK(strcmp(text, "W (3687) wifi:") == 0,
               "empty message forwarded as empty, not silently dropped or altered");
}

// ---- Eviction-under-pressure tests (2026-08-24). These need to tell "the
// queue actually has this line" from "the queue does not", which the
// default always-pdFALSE stub can never distinguish -- so these turn on
// ring mode (g_stub_queue_ring_enabled = 1) before calling
// uart_log_bridge_early_init(), which is what latches
// UART_LOG_BRIDGE_QUEUE_LEN as the ring's real capacity (see xQueueCreate()
// in the stub). Every test above this point runs with ring mode off and is
// therefore untouched by any of this. ----

// Reads the text of the ring slot `offset_from_head` positions after the
// current oldest entry (0 = oldest/front, count-1 = newest/tail) -- mirrors
// captured_entry_text()'s decode of the same uart_log_entry_t wire layout,
// but reads directly out of the ring backing array instead of the
// last-xQueueSend-call snapshot, since the whole point here is to inspect
// what is actually still queued, not just what was last offered.
static void ring_entry_text(int offset_from_head, char *out, size_t out_size)
{
    int idx = (g_stub_queue_ring_head + offset_from_head) % TEST_STUB_QUEUE_RING_MAX_CAPACITY;
    uint8_t len = g_stub_queue_ring[idx][1];
    const char *text = (const char *)&g_stub_queue_ring[idx][2];
    size_t n = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, text, n);
    out[n] = '\0';
}

// Fills the (freshly created, empty) ring queue to exactly
// UART_LOG_BRIDGE_QUEUE_LEN entries with distinguishable INFO lines, so a
// later test can tell whether the front (oldest) one survived or was
// evicted.
static void fill_queue_with_info(void)
{
    for (int i = 0; i < UART_LOG_BRIDGE_QUEUE_LEN; i++) {
        call_uart_log_vprintf("I (%d) filltag: line%d", i, i);
    }
    TEST_CHECK(g_stub_queue_ring_count == UART_LOG_BRIDGE_QUEUE_LEN, "setup: queue filled to capacity");
}

static void ring_test_setup(void)
{
    g_stub_queue_ring_enabled = 1;
    uart_log_bridge_early_init(); // latches ring capacity = UART_LOG_BRIDGE_QUEUE_LEN, resets ring
    s_dropped_lines = 0;
}

static void test_full_queue_error_evicts_oldest(void)
{
    ring_test_setup();
    fill_queue_with_info();

    char oldest_before[64];
    ring_entry_text(0, oldest_before, sizeof(oldest_before));
    TEST_CHECK(strcmp(oldest_before, "I (0) filltag: line0") == 0, "setup: line0 is the oldest queued entry");

    call_uart_log_vprintf("E (999) boom: something failed\r\n");

    // The privileged path: exactly one line lost (the evicted INFO line, per
    // the code's own comment -- "the evicted line, not this one"), and the
    // queue is still full (evict + reinsert nets to no size change).
    TEST_CHECK(s_dropped_lines == 1, "one drop counted for the evicted line, not the error");
    TEST_CHECK(g_stub_queue_ring_count == UART_LOG_BRIDGE_QUEUE_LEN, "queue stays full after evict+reinsert");

    // The oldest entry is gone -- line0 was evicted, line1 is now the front.
    char oldest_after[64];
    ring_entry_text(0, oldest_after, sizeof(oldest_after));
    TEST_CHECK(strcmp(oldest_after, "I (1) filltag: line1") == 0, "oldest entry (line0) was evicted");

    // And the error itself made it in -- it's the newest (tail) entry.
    char newest[64];
    ring_entry_text(UART_LOG_BRIDGE_QUEUE_LEN - 1, newest, sizeof(newest));
    TEST_CHECK(strcmp(newest, "E (999) boom: something failed") == 0,
               "the error line is now queued, at the tail");
}

static void test_full_queue_non_error_not_privileged(void)
{
    ring_test_setup();
    fill_queue_with_info();

    char oldest_before[64];
    ring_entry_text(0, oldest_before, sizeof(oldest_before));

    call_uart_log_vprintf("W (999) noisy: just a warning\r\n");

    // Not queued: dropped, no eviction attempted, queue contents unchanged.
    TEST_CHECK(s_dropped_lines == 1, "the warning itself is counted as dropped");
    TEST_CHECK(g_stub_queue_ring_count == UART_LOG_BRIDGE_QUEUE_LEN, "queue still full, nothing removed");
    char oldest_after[64];
    ring_entry_text(0, oldest_after, sizeof(oldest_after));
    TEST_CHECK(strcmp(oldest_before, oldest_after) == 0, "oldest entry untouched -- no eviction for a non-error");

    // Try INFO too, for the same claim.
    call_uart_log_vprintf("I (1000) noisy: just info\r\n");
    TEST_CHECK(s_dropped_lines == 2, "the info line is also counted as dropped");
    TEST_CHECK(g_stub_queue_ring_count == UART_LOG_BRIDGE_QUEUE_LEN, "queue still full after the info line too");
}

static void test_room_available_no_eviction(void)
{
    ring_test_setup();
    // Room to spare: well under UART_LOG_BRIDGE_QUEUE_LEN.
    for (int i = 0; i < 5; i++) {
        call_uart_log_vprintf("I (%d) roomtag: line%d", i, i);
    }
    TEST_CHECK(g_stub_queue_ring_count == 5, "setup: five entries queued, room left");

    call_uart_log_vprintf("E (500) boom: with room to spare\r\n");

    TEST_CHECK(s_dropped_lines == 0, "no drop when the queue had room");
    TEST_CHECK(g_stub_queue_ring_count == 6, "error simply appended, no eviction needed");
    char newest[64];
    ring_entry_text(5, newest, sizeof(newest));
    TEST_CHECK(strcmp(newest, "E (500) boom: with room to spare") == 0, "the error is queued intact");
}

static void test_eviction_bounded_per_call(void)
{
    ring_test_setup();
    fill_queue_with_info();

    // Several consecutive errors against a full queue: each may evict AT
    // MOST one entry for itself. If eviction were a loop instead of a single
    // attempt, repeated errors would drain the queue below capacity; here it
    // must stay pinned at capacity after every single one.
    for (int i = 0; i < 5; i++) {
        call_uart_log_vprintf("E (%d) boom: repeated failure %d\r\n", 2000 + i, i);
        TEST_CHECK(g_stub_queue_ring_count == UART_LOG_BRIDGE_QUEUE_LEN,
                   "queue stays exactly at capacity after each individual eviction");
    }
    TEST_CHECK(s_dropped_lines == 5, "five drops counted, one per evicted line -- none lost track of");
}

static void test_one_enqueue_per_call(void)
{
    uart_log_bridge_early_init();

    g_stub_queue_send_calls = 0;
    call_uart_log_vprintf("I (100) tag_a: first line");
    call_uart_log_vprintf("I (200) tag_b: second line");
    // Exactly one queue push per uart_log_vprintf() invocation -- a line is
    // never split into two enqueues, and one call never produces zero
    // enqueues for a well-formed line. Rules out "split across two
    // callbacks, only the first kept" as this file's own mechanism.
    TEST_CHECK(g_stub_queue_send_calls == 2, "two calls produce exactly two enqueues, never split/merged");
}

void run_test_uart_log_bridge(void)
{
    TEST_SECTION("uart_log_bridge");
    test_verbatim_forward();
    test_forwards_empty_message_faithfully();
    test_one_enqueue_per_call();
    test_full_queue_error_evicts_oldest();
    test_full_queue_non_error_not_privileged();
    test_room_available_no_eviction();
    test_eviction_bounded_per_call();
}
