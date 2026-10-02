// recovery_pico.c -- see recovery_pico.h. Protocol/validation logic is the pure,
// host-tested recovery_pico_proto.c; this file is the FreeRTOS/UART/PSRAM side.
//
// Wire behaviour relied on (SaftyFW bootloader/recovery_update.c, which is
// wire-compatible with the application receiver, ota_pico_relay.c in KilnFW):
//  - The bootloader beacons UPDATE_STATUS about once a second, polls a 32-byte
//    UART FIFO and programs flash with interrupts off, so DATA frames are
//    paced (RPP_DATA_PACE_MS) rather than streamed back to back.
//  - BEGIN erases the whole target slot synchronously; the bootloader may be
//    silent for tens of seconds, so waits for RECEIVING allow RPP_ERASE_TIMEOUT_MS.
//  - It has no timeout of its own and does not reboot itself after COMPLETE.
//    The Pico RUN pin is not wired, so success tells the operator to power-cycle
//    the board. (No SAFETY_CMD_REBOOT offer: the bootloader ignores it and the
//    application is, by definition, not what we just replaced.)
//  - Recovery never sends CLEAR_TRIP; a refusal (relay closed, trip pending,
//    too hot) is reported with its reason and nothing else.
//  - GPIO6 is never driven. UART1 uses only TX GPIO5 / RX GPIO4 and is
//    installed for the length of a transfer, then deleted and the pins reset.
#include "recovery_pico.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "recovery_pico_proto.h"

static const char *TAG = "rec_pico";

#define PICO_UART UART_NUM_1
#define PICO_TX_GPIO 5
#define PICO_RX_GPIO 4
#define PICO_BAUD 230400
#define PICO_RX_BUF 2048
#define RELAY_STACK 4096
#define RELAY_PRIO 4
// Internal heap kept free after this module's own allocations (owner decision
// 2026-10-01: heap_internal min_free >= 8192 B). The headroom covers the task
// stack, the context block and the UART driver.
#define INTERNAL_FLOOR 8192u
#define RELAY_INTERNAL_NEED (RELAY_STACK + 3072u + 4096u)

#define BEGIN_MAX_SENDS 3
#define END_MAX_ATTEMPTS 3
#define STATUS_SILENCE_LIMIT 4

typedef enum { BUSY_NONE = 0, BUSY_RESERVED = 1, BUSY_RUNNING = 2 } busy_t;

// --- shared state (httpd <-> relay task), guarded by s_lock -------------------
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_storage;

static busy_t s_busy;
static uint8_t *s_buf;
static size_t s_len;
static uint32_t s_crc;
static int s_image_slot = RPP_SLOT_UNKNOWN;
static int s_operator_slot = RPP_SLOT_UNKNOWN;
static volatile bool s_abort;
static int64_t s_last_poll_us;

static recovery_pico_phase_t s_phase;
static uint32_t s_bytes_sent;
static uint32_t s_total_bytes;
static uint32_t s_gap_count;
static uint32_t s_batches;
static int s_target_slot = RPP_SLOT_UNKNOWN;
static int s_target_source = -1;
static int s_pico_mode; // 0 unknown, 1 application, 2 bootloader
static uint8_t s_pico_state;
static uint8_t s_pico_err;
static bool s_power_cycle;
static char s_refusal[160];
static char s_error[192];
static char s_message[144];

// Task-only: never reset between uploads (the Pico's dedup state outlives one).
static uint16_t s_msg_index;

static void lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

const char *recovery_pico_phase_name(recovery_pico_phase_t p)
{
    switch (p) {
    case RECOVERY_PICO_IDLE: return "idle";
    case RECOVERY_PICO_RECEIVING: return "receiving";
    case RECOVERY_PICO_DISCOVER: return "discovering";
    case RECOVERY_PICO_BEGIN: return "begin";
    case RECOVERY_PICO_ERASING: return "erasing";
    case RECOVERY_PICO_SENDING: return "sending";
    case RECOVERY_PICO_RETRANSMIT: return "retransmit";
    case RECOVERY_PICO_FINISHING: return "finishing";
    case RECOVERY_PICO_DONE: return "done";
    case RECOVERY_PICO_FAILED: return "failed";
    case RECOVERY_PICO_ABORTED: return "aborted";
    }
    return "?";
}

bool recovery_pico_psram_available(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

// Resets the per-upload published state (the "reset one side" rule: everything
// that describes one transfer is cleared together, here, and nowhere else).
static void reset_published_locked(recovery_pico_phase_t phase)
{
    s_phase = phase;
    s_bytes_sent = 0;
    s_total_bytes = 0;
    s_gap_count = 0;
    s_batches = 0;
    s_target_slot = RPP_SLOT_UNKNOWN;
    s_target_source = -1;
    s_pico_mode = 0;
    s_pico_state = 0;
    s_pico_err = 0;
    s_power_cycle = false;
    s_refusal[0] = '\0';
    s_error[0] = '\0';
    s_message[0] = '\0';
    s_abort = false;
}

uint8_t *recovery_pico_reserve(size_t len, int *http_status, const char **why)
{
    *http_status = 503;
    if (len == 0) {
        *http_status = 400;
        *why = "empty body";
        return NULL;
    }
    if (len > RPP_SLOT_SIZE) {
        *http_status = 413;
        *why = "image larger than a Pico slot (832 KB)";
        return NULL;
    }
    if (!recovery_pico_psram_available()) {
        *why = "no PSRAM on this board: the Pico upload needs it to buffer the image";
        return NULL;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < INTERNAL_FLOOR + RELAY_INTERNAL_NEED) {
        *why = "not enough free internal memory to run the Pico relay";
        return NULL;
    }
    lock();
    if (s_busy != BUSY_NONE) {
        unlock();
        *why = "a Pico update is already in progress";
        return NULL;
    }
    s_busy = BUSY_RESERVED;
    unlock();

    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        lock();
        s_busy = BUSY_NONE;
        unlock();
        *why = "could not allocate the PSRAM image buffer";
        return NULL;
    }
    lock();
    s_buf = buf;
    s_len = len;
    reset_published_locked(RECOVERY_PICO_RECEIVING);
    s_total_bytes = (uint32_t)len;
    s_last_poll_us = esp_timer_get_time();
    unlock();
    return buf;
}

static void free_buffer_locked(void)
{
    if (s_buf) {
        heap_caps_free(s_buf);
    }
    s_buf = NULL;
    s_len = 0;
}

void recovery_pico_release(void)
{
    lock();
    if (s_busy == BUSY_RESERVED) {
        free_buffer_locked();
        s_busy = BUSY_NONE;
        s_phase = RECOVERY_PICO_IDLE;
    }
    unlock();
}

void recovery_pico_abort(void)
{
    s_abort = true;
}

// --- published-state setters (task side) ----------------------------------------

static void set_phase(recovery_pico_phase_t p)
{
    lock();
    s_phase = p;
    unlock();
}

static void set_text(char *dst, size_t cap, const char *src)
{
    lock();
    snprintf(dst, cap, "%s", src);
    unlock();
}

// --- relay context (internal heap, one per transfer) -------------------------------

typedef struct {
    rpp_rx_t rx;
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    uint8_t payload[KILNLINK_FRAME_MAX_PAYLOAD];
    rpp_status_t st;
    uint32_t st_seq; // bumps on every parsed UPDATE_STATUS
    bool app_seen;
    bool boot_seen;
    int app_active_slot;
    bool armed;      // BEGIN has been sent: terminal states are now ours
    bool fatal;      // a terminal Pico state arrived; text already published
    bool stop_text;  // abort/client-gone text already published
    int64_t next_send_us;
    uint32_t chunks;
} relay_t;

static void handle_frame(relay_t *r, const kilnlink_frame_t *f)
{
    if (f->length == 0) {
        return;
    }
    const uint8_t *p = f->payload;
    if (p[0] == RPP_CMD_GET_STATUS) {
        r->app_seen = true;
        int slot = rpp_parse_frame_a_active_slot(p, f->length);
        if (slot != RPP_SLOT_UNKNOWN) {
            r->app_active_slot = slot;
        }
        return;
    }
    if (p[0] != RPP_CMD_UPDATE_STATUS) {
        return;
    }
    rpp_status_t st;
    if (!rpp_parse_status(p, f->length, &st)) {
        return;
    }
    r->st = st;
    r->st_seq++;
    if (!r->app_seen) {
        r->boot_seen = true;
    }
    lock();
    s_pico_state = st.state;
    s_pico_err = st.err;
    s_gap_count = st.gap_count;
    unlock();

    if (!r->armed) {
        return;
    }
    switch (st.state) {
    case RPP_STATE_REFUSED:
    case RPP_STATE_REJECTED_SLOT_LINKAGE:
    case RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP: {
        char t[160];
        rpp_describe_state(st.state, st.err, t, sizeof(t));
        set_text(s_refusal, sizeof(s_refusal), t);
        set_text(s_error, sizeof(s_error), t);
        r->fatal = true;
        break;
    }
    case RPP_STATE_FAILED:
    case RPP_STATE_ABORTED: {
        char t[192];
        rpp_describe_state(st.state, st.err, t, sizeof(t));
        set_text(s_error, sizeof(s_error), t);
        r->fatal = true;
        break;
    }
    default:
        break;
    }
}

static void pump(relay_t *r)
{
    uint8_t b[64];
    int n;
    while ((n = uart_read_bytes(PICO_UART, b, sizeof(b), 0)) > 0) {
        for (int i = 0; i < n; i++) {
            kilnlink_frame_t f;
            if (rpp_rx_push(&r->rx, b[i], &f)) {
                handle_frame(r, &f);
            }
        }
    }
}

// True when the transfer must stop now: operator abort, the browser stopped
// polling, or a terminal Pico state.
static bool should_stop(relay_t *r)
{
    if (r->fatal) {
        return true;
    }
    if (!s_abort) {
        int64_t quiet_ms = (esp_timer_get_time() - s_last_poll_us) / 1000;
        if (quiet_ms > (int64_t)RPP_CLIENT_GONE_MS) {
            set_text(s_error, sizeof(s_error), "browser stopped polling status: update aborted");
            r->stop_text = true;
            s_abort = true;
        }
    }
    return s_abort;
}

static bool send_payload(relay_t *r, size_t payload_len)
{
    size_t n = rpp_build_frame(s_msg_index++, r->payload, payload_len, r->wire, sizeof(r->wire));
    if (n == 0) {
        return false;
    }
    return uart_write_bytes(PICO_UART, (const char *)r->wire, n) == (int)n;
}

// One tick-granular idle step that keeps draining the receive FIFO.
static void idle_step(relay_t *r)
{
    pump(r);
    vTaskDelay(1);
}

// Waits for a status newer than `seq`. 1 = fresh status, 0 = timeout, -1 = stop.
static int wait_status(relay_t *r, uint32_t seq, uint32_t timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        pump(r);
        if (should_stop(r)) {
            return -1;
        }
        if (r->st_seq != seq) {
            return 1;
        }
        vTaskDelay(1);
    }
    return 0;
}

// Sends chunk `index` once the pacing deadline passes. Coarse waits yield with
// vTaskDelay(1) (a 10 ms tick, so the effective pace is a little slower than
// RPP_DATA_PACE_MS -- the safe side); only the last sub-millisecond is spun.
// false = stop requested.
static bool send_chunk_paced(relay_t *r, uint32_t index)
{
    for (;;) {
        pump(r);
        if (should_stop(r)) {
            return false;
        }
        uint32_t w = rpp_pace_wait_us(esp_timer_get_time(), r->next_send_us);
        if (w == 0) {
            break;
        }
        if (w >= 1000u) {
            vTaskDelay(1);
        } else {
            esp_rom_delay_us(w);
        }
    }
    uint32_t offset = index * RPP_CHUNK_LEN;
    uint32_t n = (uint32_t)s_len - offset;
    if (n > RPP_CHUNK_LEN) {
        n = RPP_CHUNK_LEN;
    }
    size_t pl = rpp_pack_data(r->payload, offset, s_buf + offset, n);
    r->next_send_us = esp_timer_get_time() + (int64_t)RPP_DATA_PACE_MS * 1000;
    if (pl == 0 || !send_payload(r, pl)) {
        set_text(s_error, sizeof(s_error), "could not write to the Pico UART");
        r->fatal = true;
        return false;
    }
    return true;
}

// --- UART ---------------------------------------------------------------------

static bool uart_open(void)
{
    uart_config_t cfg = {
        .baud_rate = PICO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(PICO_UART, PICO_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    if (uart_param_config(PICO_UART, &cfg) != ESP_OK ||
        uart_set_pin(PICO_UART, PICO_TX_GPIO, PICO_RX_GPIO, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete(PICO_UART);
        return false;
    }
    uart_flush_input(PICO_UART);
    return true;
}

static void uart_close(void)
{
    uart_wait_tx_done(PICO_UART, pdMS_TO_TICKS(200));
    uart_driver_delete(PICO_UART);
    // Back to plain high-impedance inputs, as the board boots. GPIO6 is never
    // configured by this module.
    gpio_reset_pin(PICO_TX_GPIO);
    gpio_reset_pin(PICO_RX_GPIO);
}

// --- the transfer -----------------------------------------------------------------

static void send_abort(relay_t *r)
{
    size_t pl = rpp_pack_abort(r->payload);
    for (int i = 0; i < 2; i++) {
        (void)send_payload(r, pl);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Records a relay-side failure. Once BEGIN has gone out the Pico is told to
// ABORT (unless it already ended the transfer itself).
static void fail(relay_t *r, const char *msg)
{
    if (!r->fatal && !r->stop_text) {
        set_text(s_error, sizeof(s_error), msg);
    }
    if (r->armed && !r->fatal) {
        send_abort(r);
    }
    set_phase(RECOVERY_PICO_FAILED);
}

// Finishes a stopped transfer: ABORTED for an operator/client stop, FAILED for
// a Pico terminal state. Sends ABORT to the Pico once BEGIN has gone out.
static void finish_stopped(relay_t *r)
{
    bool fatal = r->fatal;
    if (r->armed && !(fatal && r->st.state == RPP_STATE_ABORTED)) {
        send_abort(r);
    }
    if (fatal) {
        set_phase(RECOVERY_PICO_FAILED);
    } else {
        if (!r->stop_text) {
            set_text(s_error, sizeof(s_error), "update aborted");
        }
        set_phase(RECOVERY_PICO_ABORTED);
    }
}

static bool discover(relay_t *r)
{
    set_phase(RECOVERY_PICO_DISCOVER);
    int64_t t0 = esp_timer_get_time();
    int64_t last_probe = 0;
    bool probed = false;
    while ((esp_timer_get_time() - t0) / 1000 < (int64_t)RPP_DISCOVER_TIMEOUT_MS) {
        if (should_stop(r)) {
            return false;
        }
        if (r->app_seen || r->boot_seen) {
            return true;
        }
        int64_t now = esp_timer_get_time();
        if (!probed || (now - last_probe) / 1000 >= (int64_t)RPP_DISCOVER_PROBE_MS) {
            size_t pl = rpp_pack_get_status(r->payload);
            (void)send_payload(r, pl);
            last_probe = now;
            probed = true;
        }
        idle_step(r);
    }
    if (r->app_seen || r->boot_seen) {
        return true;
    }
    set_text(s_error, sizeof(s_error), "Pico not responding - power-cycle the board and retry");
    lock();
    s_power_cycle = true;
    unlock();
    set_phase(RECOVERY_PICO_FAILED);
    return false;
}

// Returns true when the transfer completed (phase DONE).
static bool run_transfer(relay_t *r)
{
    if (!discover(r)) {
        return false;
    }
    lock();
    s_pico_mode = r->app_seen ? 1 : 2;
    unlock();

    // Target-slot resolution, before anything is erased (see the proto header).
    rpp_target_t t = rpp_resolve_target(r->app_seen ? r->app_active_slot : RPP_SLOT_UNKNOWN,
                                        s_operator_slot);
    lock();
    s_target_slot = t.target_slot;
    s_target_source = (int)t.source;
    unlock();
    if (!rpp_image_matches_target(s_image_slot, t)) {
        char m[192];
        snprintf(m, sizeof(m),
                 "image is linked for slot %c but the Pico will write slot %c (%s): use "
                 "SaftyFW_slot%c.bin or choose the other slot",
                 s_image_slot == RPP_SLOT_A ? 'A' : 'B', t.target_slot == RPP_SLOT_A ? 'A' : 'B',
                 t.source == RPP_TARGET_FROM_APP     ? "read from the Pico application"
                 : t.source == RPP_TARGET_OPERATOR   ? "as you selected"
                                                     : "assumed: slot cannot be read from the bootloader",
                 t.target_slot == RPP_SLOT_A ? 'A' : 'B');
        set_text(s_error, sizeof(s_error), m);
        set_phase(RECOVERY_PICO_FAILED);
        return false;
    }

    // BEGIN
    set_phase(RECOVERY_PICO_BEGIN);
    r->chunks = rpp_chunk_count((uint32_t)s_len);
    r->armed = true;
    int begin_sends = 1;
    size_t pl = rpp_pack_begin(r->payload, (uint32_t)s_len, s_crc, "recovery");
    if (pl == 0 || !send_payload(r, pl)) {
        fail(r, "could not write BEGIN to the Pico UART");
        return false;
    }
    int64_t t_begin = esp_timer_get_time();
    uint32_t seen = r->st_seq;
    uint32_t idle_count = 0;
    bool receiving = false;
    while ((esp_timer_get_time() - t_begin) / 1000 < (int64_t)RPP_ERASE_TIMEOUT_MS) {
        pump(r);
        if (should_stop(r)) {
            return false;
        }
        if (r->st_seq != seen) {
            seen = r->st_seq;
            if (r->st.state == RPP_STATE_RECEIVING) {
                receiving = true;
                break;
            }
            if (r->st.state == RPP_STATE_ERASING) {
                set_phase(RECOVERY_PICO_ERASING);
            } else if (r->st.state == RPP_STATE_IDLE) {
                idle_count++;
            }
        }
        int64_t waited_ms = (esp_timer_get_time() - t_begin) / 1000;
        if (waited_ms > (int64_t)RPP_BEGIN_REPLY_TIMEOUT_MS) {
            // Silent for the reply window: a bootloader erases without answering.
            lock();
            if (s_phase == RECOVERY_PICO_BEGIN) {
                s_phase = RECOVERY_PICO_ERASING;
            }
            unlock();
        }
        if (idle_count >= 2 && begin_sends < BEGIN_MAX_SENDS) {
            // Two idle beacons after BEGIN: the Pico never saw it. Resend.
            (void)send_payload(r, pl);
            begin_sends++;
            idle_count = 0;
        }
        vTaskDelay(1);
    }
    if (!receiving) {
        lock();
        s_power_cycle = true;
        unlock();
        fail(r, "Pico did not start receiving (erase timed out or BEGIN ignored): power-cycle "
                "the board and retry");
        return false;
    }
    if (r->st.total_chunks != 0 && r->st.total_chunks != r->chunks) {
        char m[96];
        snprintf(m, sizeof(m), "Pico expects %u chunks but the image has %u",
                 (unsigned)r->st.total_chunks, (unsigned)r->chunks);
        fail(r, m);
        return false;
    }

    // First pass: every chunk in order, paced.
    set_phase(RECOVERY_PICO_SENDING);
    r->next_send_us = esp_timer_get_time();
    for (uint32_t i = 0; i < r->chunks; i++) {
        if (!send_chunk_paced(r, i)) {
            return false;
        }
        uint32_t sent = (i + 1u) * RPP_CHUNK_LEN;
        lock();
        s_bytes_sent = sent > (uint32_t)s_len ? (uint32_t)s_len : sent;
        unlock();
    }

    // Gap rounds, then END (a few attempts: END can be answered with RECEIVING
    // again when the Pico still lists gaps).
    for (int attempt = 0; attempt < END_MAX_ATTEMPTS; attempt++) {
        rpp_gap_tracker_t gt;
        rpp_gap_tracker_init(&gt);
        int silent = 0;
        for (;;) {
            pump(r);
            uint32_t seq0 = r->st_seq;
            int w = wait_status(r, seq0, RPP_GAP_ROUND_WAIT_MS);
            if (w < 0) {
                return false;
            }
            if (w == 0) {
                if (++silent >= STATUS_SILENCE_LIMIT) {
                    fail(r, "Pico stopped reporting status during the transfer");
                    return false;
                }
                continue;
            }
            silent = 0;
            if (r->st.state != RPP_STATE_RECEIVING) {
                if (r->st.state == RPP_STATE_IDLE) {
                    fail(r, "Pico left the transfer (reset?): power-cycle and retry");
                    return false;
                }
                continue; // VERIFYING etc: keep waiting
            }
            rpp_gap_action_t act = rpp_gap_next(&gt, &r->st);
            lock();
            s_batches = gt.batches;
            unlock();
            if (act == RPP_GAP_FAIL) {
                fail(r, "Pico did not accept the missing chunks (no progress)");
                return false;
            }
            if (act == RPP_GAP_SEND_END) {
                break;
            }
            if (act == RPP_GAP_RETRANSMIT) {
                set_phase(RECOVERY_PICO_RETRANSMIT);
                r->next_send_us = esp_timer_get_time();
                for (uint8_t g = 0; g < r->st.gap_count; g++) {
                    uint32_t idx = r->st.gaps[g];
                    if (idx >= r->chunks) {
                        continue;
                    }
                    if (!send_chunk_paced(r, idx)) {
                        return false;
                    }
                }
                pump(r); // consume any status that arrived mid-burst
            }
        }

        // END
        set_phase(RECOVERY_PICO_FINISHING);
        size_t el = rpp_pack_end(r->payload, s_crc);
        if (el == 0 || !send_payload(r, el)) {
            fail(r, "could not write END to the Pico UART");
            return false;
        }
        uint32_t seq1 = r->st_seq;
        int64_t t_end = esp_timer_get_time();
        bool again = false;
        while ((esp_timer_get_time() - t_end) / 1000 < (int64_t)RPP_END_REPLY_TIMEOUT_MS) {
            int w = wait_status(r, seq1, 500);
            if (w < 0) {
                return false;
            }
            if (w == 0) {
                continue;
            }
            seq1 = r->st_seq;
            if (r->st.state == RPP_STATE_COMPLETE) {
                set_phase(RECOVERY_PICO_DONE);
                lock();
                s_power_cycle = true;
                s_bytes_sent = (uint32_t)s_len;
                snprintf(s_message, sizeof(s_message),
                         "Pico firmware written and verified. Power-cycle the board to run it "
                         "(the Pico reset line is not wired).");
                unlock();
                return true;
            }
            if (r->st.state == RPP_STATE_RECEIVING) {
                again = true; // END arrived with chunks still missing
                break;
            }
        }
        if (!again) {
            continue; // no answer: resend END via the next attempt
        }
    }
    fail(r, "Pico did not confirm completion");
    return false;
}

static void relay_task(void *arg)
{
    relay_t *r = (relay_t *)arg;

    if (!uart_open()) {
        fail(r, "could not open the Pico UART");
    } else {
        pump(r);
        if (!run_transfer(r) && (r->fatal || s_abort)) {
            finish_stopped(r);
        }
        uart_close();
    }
    ESP_LOGI(TAG, "relay finished, phase=%s", recovery_pico_phase_name(s_phase));
    free(r);
    lock();
    free_buffer_locked();
    s_busy = BUSY_NONE;
    unlock();
    vTaskDelete(NULL);
}

bool recovery_pico_start(size_t len, uint32_t crc32, int image_slot, int operator_slot,
                         const char **why)
{
    relay_t *r = calloc(1, sizeof(*r));
    if (!r) {
        recovery_pico_release();
        *why = "out of internal memory";
        return false;
    }
    rpp_rx_init(&r->rx);
    r->app_active_slot = RPP_SLOT_UNKNOWN;

    lock();
    s_len = len;
    s_crc = crc32;
    s_image_slot = image_slot;
    s_operator_slot = operator_slot;
    s_abort = false;
    s_last_poll_us = esp_timer_get_time();
    s_phase = RECOVERY_PICO_DISCOVER;
    s_busy = BUSY_RUNNING;
    unlock();

    if (xTaskCreate(relay_task, "rec_pico", RELAY_STACK, r, RELAY_PRIO, NULL) != pdPASS) {
        free(r);
        lock();
        free_buffer_locked();
        s_busy = BUSY_NONE;
        s_phase = RECOVERY_PICO_IDLE;
        unlock();
        *why = "could not start the Pico relay task";
        return false;
    }
    return true;
}

// Minimal JSON string escape into out; returns the new write position.
static size_t json_str(char *out, size_t cap, size_t pos, const char *s)
{
    if (pos < cap) {
        out[pos++] = '"';
    }
    for (; *s && pos + 2 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c < 0x20) {
            out[pos++] = ' ';
        } else {
            out[pos++] = (char)c;
        }
    }
    if (pos < cap) {
        out[pos++] = '"';
    }
    return pos;
}

int recovery_pico_status_json(char *out, size_t cap)
{
    char refusal[sizeof(s_refusal)], error[sizeof(s_error)], message[sizeof(s_message)];
    recovery_pico_phase_t phase;
    uint32_t sent, total, gaps, batches;
    int tslot, tsrc, mode;
    uint8_t pstate, perr;
    bool pc, busy;

    lock();
    phase = s_phase;
    sent = s_bytes_sent;
    total = s_total_bytes;
    gaps = s_gap_count;
    batches = s_batches;
    tslot = s_target_slot;
    tsrc = s_target_source;
    mode = s_pico_mode;
    pstate = s_pico_state;
    perr = s_pico_err;
    pc = s_power_cycle;
    busy = s_busy != BUSY_NONE;
    memcpy(refusal, s_refusal, sizeof(refusal));
    memcpy(error, s_error, sizeof(error));
    memcpy(message, s_message, sizeof(message));
    if (busy) {
        s_last_poll_us = esp_timer_get_time(); // the page is still watching
    }
    unlock();

    static const char *const modes[] = {"unknown", "application", "bootloader"};
    static const char *const srcs[] = {"pico-app", "operator", "assumed-default"};
    int n = snprintf(out, cap,
                     "{\"phase\":\"%s\",\"busy\":%s,\"psram\":%s,\"bytes_sent\":%u,"
                     "\"total_bytes\":%u,\"gap_count\":%u,\"gap_rounds\":%u,"
                     "\"pico_mode\":\"%s\",\"pico_state\":%u,\"pico_err\":%u,"
                     "\"target_slot\":\"%s\",\"target_source\":\"%s\",\"power_cycle\":%s,"
                     "\"refusal\":",
                     recovery_pico_phase_name(phase), busy ? "true" : "false",
                     recovery_pico_psram_available() ? "true" : "false", (unsigned)sent,
                     (unsigned)total, (unsigned)gaps, (unsigned)batches, modes[mode >= 0 && mode < 3 ? mode : 0],
                     (unsigned)pstate, (unsigned)perr,
                     tslot == RPP_SLOT_A ? "A" : tslot == RPP_SLOT_B ? "B" : "unknown",
                     tsrc >= 0 && tsrc < 3 ? srcs[tsrc] : "unknown", pc ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    size_t pos = (size_t)n;
    pos = json_str(out, cap, pos, refusal);
    n = snprintf(out + pos, cap - pos, ",\"error\":");
    pos += (size_t)n;
    pos = json_str(out, cap, pos, error);
    n = snprintf(out + pos, cap - pos, ",\"message\":");
    pos += (size_t)n;
    pos = json_str(out, cap, pos, message);
    if (pos + 2 >= cap) {
        return 0;
    }
    out[pos++] = '}';
    out[pos] = '\0';
    return (int)pos;
}
