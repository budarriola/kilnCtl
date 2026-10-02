// recovery_pico.c -- see recovery_pico.h. Protocol/validation logic is the pure,
// host-tested recovery_pico_proto.c; this file is the FreeRTOS/UART/PSRAM side.
//
// Wire behaviour relied on (SaftyFW bootloader/recovery_update.c, which is
// wire-compatible with the application receiver, ota_pico_relay.c in KilnFW):
//  - The bootloader beacons UPDATE_STATUS about once a second, polls a 32-byte
//    UART FIFO and programs flash with interrupts off, so DATA frames are
//    paced (RPP_DATA_PACE_MS) rather than streamed back to back. The
//    application receiver instead has a 4-deep queue drained every ~100 ms
//    that drops frames when full, so it gets the slower RPP_DATA_PACE_APP_MS
//    (rpp_data_pace_ms() picks by the mode discover() saw).
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
static char s_message[192];

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
    case RECOVERY_PICO_UNKNOWN: return "outcome_unknown";
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

// --- relay context (PSRAM, one per transfer) ---------------------------------------
// The deframer and the wire/payload buffers are the only large pieces; they live
// in PSRAM (heap_caps_calloc, MALLOC_CAP_SPIRAM) so the relay costs internal RAM
// only for its task stack and the UART driver.

typedef struct {
    rpp_rx_t rx;
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    uint8_t payload[KILNLINK_FRAME_MAX_PAYLOAD];
    rpp_status_t st;
    uint32_t st_seq; // bumps on every parsed UPDATE_STATUS
    bool app_seen;
    bool boot_seen;
    int app_active_slot;
    int64_t app_first_ms; // when the application first answered (-1 = not yet)
    bool armed;      // BEGIN has been sent: terminal states are now ours
    bool fatal;      // a terminal Pico state arrived; text already published
    bool stop_text;  // abort/client-gone text already published
    bool unknown;    // END was sent and the outcome was lost: no ABORT, operator must check
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
    // Only an idle report proves a bootloader: a busy application with a
    // stale transfer can also answer GET_STATUS with UPDATE_STATUS.
    if (!r->app_seen && rpp_status_proves_bootloader(&st)) {
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
        char t[256];
        rpp_describe_state(st.state, st.err, t, sizeof(t));
        set_text(s_refusal, sizeof(s_refusal), t);
        set_text(s_error, sizeof(s_error), t);
        if (st.err & RPP_ERR_TRIP_PENDING) {
            // Recovery never sends CLEAR_TRIP; a power-cycle re-arms the
            // application or boots the Pico's bootloader.
            lock();
            s_power_cycle = true;
            unlock();
        }
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

// Waits out the pacing deadline of the previous frame. The delay is whole RTOS
// ticks rounded UP (rpp_pace_delay_ticks), never a shortfall; the loop re-checks
// the deadline each pass, and only the last sub-millisecond is spun. Applies to
// every frame that follows a DATA frame (DATA, END), and the deadline is NOT
// reset between rounds. false = stop requested.
static bool pace_wait(relay_t *r)
{
    for (;;) {
        pump(r);
        if (should_stop(r)) {
            return false;
        }
        uint32_t w = rpp_pace_wait_us(esp_timer_get_time(), r->next_send_us);
        if (w == 0) {
            return true;
        }
        if (w >= 1000u) {
            vTaskDelay(rpp_pace_delay_ticks(w, (uint32_t)portTICK_PERIOD_MS * 1000u));
        } else {
            esp_rom_delay_us(w);
        }
    }
}

// Sends chunk `index` once the pacing deadline passes. false = stop requested.
static bool send_chunk_paced(relay_t *r, uint32_t index)
{
    if (!pace_wait(r)) {
        return false;
    }
    uint32_t offset = index * RPP_CHUNK_LEN;
    uint32_t n = (uint32_t)s_len - offset;
    if (n > RPP_CHUNK_LEN) {
        n = RPP_CHUNK_LEN;
    }
    size_t pl = rpp_pack_data(r->payload, offset, s_buf + offset, n);
    r->next_send_us =
        esp_timer_get_time() + (int64_t)rpp_data_pace_ms(!r->app_seen) * 1000;
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

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool send_probe(relay_t *r)
{
    // ANNOUNCE_VERSION makes the application publish Frame A V3 (its active
    // slot); a bootloader ignores it. GET_STATUS draws a reply from either.
    size_t pl = rpp_pack_announce(r->payload);
    bool ok = send_payload(r, pl);
    pl = rpp_pack_get_status(r->payload);
    return send_payload(r, pl) && ok;
}

static bool discover(relay_t *r)
{
    set_phase(RECOVERY_PICO_DISCOVER);
    int64_t t0 = now_ms();
    int64_t last_probe = 0;
    bool probed = false;
    while (now_ms() - t0 < (int64_t)RPP_DISCOVER_TIMEOUT_MS) {
        if (should_stop(r)) {
            return false;
        }
        int64_t now = now_ms();
        if (r->boot_seen) {
            return true;
        }
        if (r->app_seen) {
            if (r->app_active_slot != RPP_SLOT_UNKNOWN) {
                return true;
            }
            if (r->app_first_ms < 0) {
                r->app_first_ms = now;
            } else if (now - r->app_first_ms >= (int64_t)RPP_APP_SLOT_WAIT_MS) {
                return true; // slot never reported: the operator must name it
            }
        }
        if (!probed || now - last_probe >= (int64_t)RPP_DISCOVER_PROBE_MS) {
            (void)send_probe(r);
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

static void publish_done(bool operator_target)
{
    set_phase(RECOVERY_PICO_DONE);
    lock();
    s_power_cycle = true;
    s_bytes_sent = (uint32_t)s_len;
    snprintf(s_message, sizeof(s_message),
             "Pico firmware written and verified%s. Power-cycle the board to run it (Pico "
             "reset line is not wired).",
             operator_target ? " (target unverified: slot chosen by the operator)" : "");
    unlock();
}

// BEGIN, then wait for RECEIVING through the (possibly silent) erase.
static bool begin_transfer(relay_t *r)
{
    set_phase(RECOVERY_PICO_BEGIN);
    size_t pl = rpp_pack_begin(r->payload, (uint32_t)s_len, s_crc, "recovery");
    if (pl == 0 || !send_payload(r, pl)) {
        fail(r, "could not write BEGIN to the Pico UART");
        return false;
    }
    rpp_begin_t bg;
    rpp_begin_start(&bg, now_ms());
    int64_t t_begin = now_ms();
    uint32_t seen = r->st_seq;
    for (;;) {
        pump(r);
        if (should_stop(r)) {
            return false;
        }
        int64_t now = now_ms();
        rpp_begin_action_t a;
        if (r->st_seq != seen) {
            seen = r->st_seq;
            a = rpp_begin_step(&bg, &r->st, now);
        } else {
            a = rpp_begin_step(&bg, NULL, now);
        }
        if (a == RPP_BEGIN_RECEIVING) {
            return true;
        }
        if (a == RPP_BEGIN_FAIL) {
            lock();
            s_power_cycle = true;
            unlock();
            fail(r, "Pico did not start receiving (erase timed out or BEGIN ignored): "
                    "power-cycle the board and retry");
            return false;
        }
        if (a == RPP_BEGIN_ERASING) {
            set_phase(RECOVERY_PICO_ERASING);
        } else if (a == RPP_BEGIN_RESEND) {
            // Several IDLE beacons and the minimum wait: the Pico never saw BEGIN.
            (void)send_payload(r, pl);
        } else if (now - t_begin > (int64_t)RPP_BEGIN_REPLY_TIMEOUT_MS) {
            // Silent for the reply window: a bootloader erases without answering.
            lock();
            if (s_phase == RECOVERY_PICO_BEGIN) {
                s_phase = RECOVERY_PICO_ERASING;
            }
            unlock();
        }
        vTaskDelay(1);
    }
}

#define STOP_AFTER_END_TEXT \
    "stopped after END was sent - outcome unknown: power-cycle and check the Pico version; do NOT retry blindly"

// END was sent and its result never arrived (or the operator/browser stopped
// us after END): the Pico may already have committed the image. Do not send
// ABORT -- it cannot revert a committed slot (it is a no-op once the receiver
// is inactive), and sending it would only let this report claim "aborted" for
// an image that may be live. Never call it FAILED or ABORTED either: say so and
// have the operator power-cycle and check the Pico's version before any retry.
static void finish_unknown(relay_t *r, const char *why)
{
    set_text(s_error, sizeof(s_error),
             why ? why : "outcome unknown - power-cycle and check the Pico version");
    lock();
    s_power_cycle = true;
    unlock();
    r->unknown = true;
    set_phase(RECOVERY_PICO_UNKNOWN);
}

// END, gap rounds and completion: driven by rpp_fin_step (see the proto header
// for why the ESP must send END itself -- the receivers go silent once every
// chunk is in).
static bool finish_transfer(relay_t *r)
{
    set_phase(RECOVERY_PICO_FINISHING);
    rpp_fin_t f;
    rpp_fin_start(&f);
    rpp_fin_action_t act = RPP_FIN_SEND_END;
    rpp_status_t cur;
    memset(&cur, 0, sizeof(cur));
    uint32_t seen = r->st_seq;
    int64_t t_end = now_ms();
    for (;;) {
        if (act == RPP_FIN_DONE) {
            return true;
        }
        if (act == RPP_FIN_FAIL) {
            fail(r, f.why ? f.why : "Pico did not confirm completion");
            return false;
        }
        if (act == RPP_FIN_UNKNOWN) {
            finish_unknown(r, f.why);
            return false;
        }
        if (act == RPP_FIN_RETRANSMIT) {
            set_phase(RECOVERY_PICO_RETRANSMIT);
            lock();
            s_batches = f.batches;
            unlock();
            for (uint8_t g = 0; g < cur.gap_count; g++) {
                uint32_t idx = cur.gaps[g];
                if (idx >= r->chunks) {
                    continue;
                }
                if (!send_chunk_paced(r, idx)) {
                    if (rpp_fin_stop_is_unknown(&f, r->fatal)) {
                        finish_unknown(r, STOP_AFTER_END_TEXT);
                    }
                    return false;
                }
            }
        }
        if (act == RPP_FIN_SEND_END || act == RPP_FIN_RETRANSMIT) {
            set_phase(RECOVERY_PICO_FINISHING);
            // END is a frame too: honour the pace after the last DATA, and treat
            // every status queued before it as history (only its reply counts).
            if (!pace_wait(r)) {
                if (rpp_fin_stop_is_unknown(&f, r->fatal)) {
                    finish_unknown(r, STOP_AFTER_END_TEXT);
                }
                return false;
            }
            pump(r);
            seen = r->st_seq;
            size_t el = rpp_pack_end(r->payload, s_crc);
            if (el == 0 || !send_payload(r, el)) {
                fail(r, "could not write END to the Pico UART");
                return false;
            }
            rpp_fin_end_sent(&f);
            t_end = now_ms();
        }
        int w = wait_status(r, seen, RPP_GAP_ROUND_WAIT_MS);
        if (w < 0) {
            // Stopped after END went out (operator Abort, browser gone): the
            // Pico may have committed. A Pico-terminal-state stop (fatal) is
            // a real answer and keeps its own report.
            if (rpp_fin_stop_is_unknown(&f, r->fatal)) {
                finish_unknown(r, STOP_AFTER_END_TEXT);
            }
            return false;
        }
        if (w > 0) {
            seen = r->st_seq;
            cur = r->st;
        }
        act = rpp_fin_step(&f, w > 0 ? RPP_EV_STATUS : RPP_EV_QUIET, w > 0 ? &cur : NULL,
                           (uint32_t)(now_ms() - t_end));
        if (f.restart_timer) {
            t_end = now_ms();
            f.restart_timer = false;
        }
    }
}

// Returns true when the transfer completed (phase DONE).
static bool run_transfer(relay_t *r)
{
    if (!discover(r)) {
        return false;
    }
    bool bootloader = !r->app_seen;
    lock();
    s_pico_mode = bootloader ? 2 : 1;
    unlock();

    // Target-slot resolution, before anything is erased (see the proto header).
    // Never assumed: the bootloader cannot report its slot, so it always needs
    // the operator's choice.
    rpp_target_t t = rpp_resolve_target(bootloader ? RPP_SLOT_UNKNOWN : r->app_active_slot,
                                        s_operator_slot);
    lock();
    s_target_slot = t.target_slot;
    s_target_source = (int)t.source;
    unlock();
    char m[240];
    if (rpp_describe_target_refusal(t, s_image_slot, bootloader, m, sizeof(m))) {
        set_text(s_refusal, sizeof(s_refusal), m);
        set_text(s_error, sizeof(s_error), m);
        set_phase(RECOVERY_PICO_FAILED);
        return false;
    }
    if (t.source == RPP_TARGET_OPERATOR) {
        snprintf(m, sizeof(m),
                 "target unverified: writing slot %c as you chose; the Pico cannot confirm it. A "
                 "wrong choice leaves it unbootable until SWD.",
                 t.target_slot == RPP_SLOT_A ? 'A' : 'B');
        set_text(s_message, sizeof(s_message), m);
    }

    r->chunks = rpp_chunk_count((uint32_t)s_len);
    r->armed = true;
    if (!begin_transfer(r)) {
        return false;
    }
    if (r->st.total_chunks != 0 && r->st.total_chunks != r->chunks) {
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

    if (!finish_transfer(r)) {
        return false;
    }
    publish_done(t.source == RPP_TARGET_OPERATOR);
    return true;
}

static bool internal_ram_ok(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= INTERNAL_FLOOR;
}

static void relay_task(void *arg)
{
    relay_t *r = (relay_t *)arg;
    bool uart_up = false;

    if (!internal_ram_ok()) {
        // The task stack itself has just been allocated: re-check the floor.
        fail(r, "not enough free internal memory to run the Pico relay");
    } else if (!uart_open()) {
        fail(r, "could not open the Pico UART");
    } else {
        uart_up = true;
        if (!internal_ram_ok()) {
            fail(r, "not enough free internal memory to run the Pico relay (UART driver)");
        } else {
            pump(r);
            if (!run_transfer(r) && !r->unknown && (r->fatal || s_abort)) {
                finish_stopped(r);
            }
        }
    }
    if (uart_up) {
        uart_close();
    }
    ESP_LOGI(TAG, "relay finished, phase=%s, stack high-water %u B free",
             recovery_pico_phase_name(s_phase),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    heap_caps_free(r);
    lock();
    free_buffer_locked();
    s_busy = BUSY_NONE;
    unlock();
    vTaskDelete(NULL);
}

static char *json_buf_locked(void); // status JSON buffer, defined with the JSON builder

bool recovery_pico_start(size_t len, uint32_t crc32, int image_slot, int operator_slot,
                         const char **why)
{
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) <
        INTERNAL_FLOOR + RELAY_STACK + PICO_RX_BUF + 1024u) {
        recovery_pico_release();
        *why = "not enough free internal memory to run the Pico relay";
        return false;
    }
    relay_t *r = heap_caps_calloc(1, sizeof(*r), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) {
        recovery_pico_release();
        *why = "out of PSRAM for the relay buffers";
        return false;
    }
    rpp_rx_init(&r->rx);
    r->app_active_slot = RPP_SLOT_UNKNOWN;
    r->app_first_ms = -1;
    if (!internal_ram_ok()) {
        heap_caps_free(r);
        recovery_pico_release();
        *why = "not enough free internal memory to run the Pico relay";
        return false;
    }

    lock();
    s_len = len;
    s_crc = crc32;
    s_image_slot = image_slot;
    s_operator_slot = operator_slot;
    s_abort = false;
    s_last_poll_us = esp_timer_get_time();
    (void)json_buf_locked(); // pre-allocate the status buffer (best effort; retried lazily)
    s_phase = RECOVERY_PICO_DISCOVER;
    s_busy = BUSY_RUNNING;
    unlock();

    if (xTaskCreate(relay_task, "rec_pico", RELAY_STACK, r, RELAY_PRIO, NULL) != pdPASS) {
        heap_caps_free(r);
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

// --- status JSON ----------------------------------------------------------------------
// Built into one PSRAM buffer while holding the relay lock, straight from the
// published state: no stack copies of the text fields. The buffer is allocated
// by recovery_pico_start() (under the lock) and, if that failed or the status
// is read before any start, lazily under the same lock -- never outside it.
// SINGLE READER: only the httpd task calls recovery_pico_status_json()
// (the recovery server runs one httpd task); the buffer is consumed (sent)
// before the handler returns, so one static buffer is enough. A second
// concurrent reader would overwrite it mid-send.
#define JSON_CAP 1536
static char *s_json;

// Caller holds the lock.
static char *json_buf_locked(void)
{
    if (!s_json) {
        s_json = heap_caps_malloc(JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return s_json;
}

// Minimal JSON string escape into out; returns the new write position, or
// (size_t)-1 when it does not fit.
static size_t json_str(char *out, size_t cap, size_t pos, const char *s)
{
    if (pos + 1 >= cap) {
        return (size_t)-1;
    }
    out[pos++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (pos + 3 >= cap) { // room for an escape pair plus the closing quote
            return (size_t)-1;
        }
        if (c == '"' || c == '\\') {
            out[pos++] = '\\';
            out[pos++] = (char)c;
        } else if (c < 0x20) {
            out[pos++] = ' ';
        } else {
            out[pos++] = (char)c;
        }
    }
    out[pos++] = '"';
    return pos;
}

// Appends a snprintf result, clamping: false when it would not fit.
static bool json_fmt(char *out, size_t cap, size_t *pos, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *pos) {
        return false;
    }
    *pos += (size_t)n;
    return true;
}

bool recovery_pico_busy(void)
{
    return s_busy != BUSY_NONE;
}

const char *recovery_pico_status_json(int *len)
{
    static const char no_psram[] = "{\"phase\":\"idle\",\"busy\":false,\"psram\":false}";
    *len = 0;
    if (!recovery_pico_psram_available()) {
        *len = (int)(sizeof(no_psram) - 1);
        return no_psram;
    }
    static const char *const modes[] = {"unknown", "application", "bootloader"};
    static const char *const srcs[] = {"pico-app", "operator", "unresolved"};
    size_t pos = 0;
    bool ok;

    lock();
    char *out = json_buf_locked();
    if (!out) {
        unlock();
        return NULL;
    }
    bool busy = s_busy != BUSY_NONE;
    if (busy) {
        s_last_poll_us = esp_timer_get_time(); // the page is still watching
    }
    int mode = s_pico_mode;
    int tsrc = s_target_source;
    ok = json_fmt(out, JSON_CAP, &pos,
                  "{\"phase\":\"%s\",\"busy\":%s,\"psram\":true,\"bytes_sent\":%u,"
                  "\"total_bytes\":%u,\"gap_count\":%u,\"gap_rounds\":%u,"
                  "\"pico_mode\":\"%s\",\"pico_state\":%u,\"pico_err\":%u,"
                  "\"target_slot\":\"%s\",\"target_source\":\"%s\",\"power_cycle\":%s,"
                  "\"refusal\":",
                  recovery_pico_phase_name(s_phase), busy ? "true" : "false", (unsigned)s_bytes_sent,
                  (unsigned)s_total_bytes, (unsigned)s_gap_count, (unsigned)s_batches,
                  modes[mode >= 0 && mode < 3 ? mode : 0], (unsigned)s_pico_state,
                  (unsigned)s_pico_err,
                  s_target_slot == RPP_SLOT_A ? "A" : s_target_slot == RPP_SLOT_B ? "B" : "unknown",
                  tsrc >= 0 && tsrc < 3 ? srcs[tsrc] : "unknown", s_power_cycle ? "true" : "false");
    if (ok) {
        pos = json_str(out, JSON_CAP, pos, s_refusal);
        ok = pos != (size_t)-1 && json_fmt(out, JSON_CAP, &pos, ",\"error\":");
    }
    if (ok) {
        pos = json_str(out, JSON_CAP, pos, s_error);
        ok = pos != (size_t)-1 && json_fmt(out, JSON_CAP, &pos, ",\"message\":");
    }
    if (ok) {
        pos = json_str(out, JSON_CAP, pos, s_message);
        ok = pos != (size_t)-1 && json_fmt(out, JSON_CAP, &pos, "}");
    }
    unlock();
    if (!ok) {
        return NULL;
    }
    *len = (int)pos;
    return out;
}
