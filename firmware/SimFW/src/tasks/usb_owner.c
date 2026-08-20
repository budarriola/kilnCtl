// usb_owner.c -- see usb_owner.h. TinyUSB CDC init, a SLIP-style byte-stream
// reader/writer for `benchproto_frame`'s codec, and the delivery/dedup/
// task-registration decisions from `benchproto_link` (docs/PLAN.md section
// 4.1, firmware/CommonFW/docs/BENCHPROTO.md). This is the ONE place that
// touches the USB peripheral (single-owner doctrine, PLAN.md section 4's
// opening paragraph) -- cmd_task never includes tusb.h or benchproto_link.h;
// it only calls usb_owner_register_task()/usb_owner_send_reply()
// (usb_owner.h) and reads from the queue usb_owner posts delivered requests
// to (cmd_task_get_inbox(), cmd_task.h).
//
// RX byte-assembly (delimiter-bounded, bounded buffer, resync-on-0x7E from
// any state) mirrors ../../SaftyFW/src/tasks/link_task.c's
// link_task_rx_process_byte()/link_task_handle_raw_frame() shape almost
// exactly -- same reasoning (LINK_PROTOCOL.md/BENCHPROTO.md's framing
// section: "a delimiter is always a frame boundary, whatever came before
// it"), just reading from TinyUSB CDC instead of a UART IRQ ring, and
// dispatching through benchproto's addressed request/reply model instead of
// kilnlink's fixed BROADCAST-only status frames.
//
// Reply convention (docs/PROTOCOL.md): this build piggybacks the actual
// command reply payload directly in the frame's ACK, rather than
// UnitTestFw's older two-frame ACK-then-separate-DATA INFO pattern
// (UnitTestFw/UnitTest/docs/UART_PROTOCOL.md's INFO section) -- simpler, and
// nothing in BENCHPROTO.md's request/reply description (section 3: "waits
// for a reply (ACK/NACK)") requires the two-frame split. See
// usb_owner_send_reply()'s doc comment in usb_owner.h.
#include "usb_owner.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "tusb.h"

#include "benchproto/benchproto_frame.h"
#include "benchproto/benchproto_link.h"

#include "cmd_ids.h"
#include "cmd_task.h"
#include "task_priorities.h"

// Bumped from configMINIMAL_STACK_SIZE (the skeleton's own placeholder):
// the encode/transmit path puts a raw[BENCHPROTO_FRAME_RAW_MAX] (~138 bytes)
// and a stuffed[BENCHPROTO_FRAME_STUFFED_MAX] (~278 bytes) buffer on the
// stack, on top of TinyUSB's own tud_task()/tusb_init() call depth -- same
// "two ~500-byte buffers no longer comfortably fit configMINIMAL_STACK_SIZE
// alone" reasoning SaftyFW's link_task.c documents for its own RX assembly
// buffers.
#define USB_OWNER_STACK_WORDS (configMINIMAL_STACK_SIZE * 4)

// Bounded wait for the TX mutex (guards the shared CDC write path and the
// per-task ACK cache below) -- never an unbounded block, matching every
// other mutex-take in this codebase (e.g. SaftyFW's link_task_publish_context()
// 50ms wait). A single struct copy plus a small tud_cdc_write() call should
// never contend long enough for this to matter in practice.
#define USB_OWNER_TX_LOCK_WAIT_MS 50u

// One slot per SIMFW_TASK_ID_* (cmd_ids.h), index 0 unused (task ids start
// at 1) -- small, fixed, and avoids a search. Each slot caches the last ACK
// sent in reply to that task's last request, so a
// BENCHPROTO_LINK_ACTION_DUPLICATE_REACK (the host's retry outran the first
// ACK) can resend the *exact* same bytes rather than recomputing a reply
// that might have changed in the meantime (BENCHPROTO_LINK_ACTION_DELIVER's
// own doc comment: "Caller must NOT redeliver ... just resend ACK").
#define USB_OWNER_ACK_CACHE_SLOTS (SIMFW_TASK_ID_EVT + 1u)

typedef struct {
    bool valid;
    uint8_t stuffed[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t len;
} usb_owner_ack_cache_slot_t;

static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_tx_lock = NULL;

static benchproto_link_t s_link;
static usb_owner_ack_cache_slot_t s_ack_cache[USB_OWNER_ACK_CACHE_SLOTS];

// RX assembly: collects STUFFED wire bytes between 0x7E delimiters, same
// shape as SaftyFW's link_task.c s_rx_assembly/s_rx_assembly_len/
// s_rx_collecting. Single-writer (this task's own RX loop) -- no lock
// needed.
static uint8_t s_rx_assembly[BENCHPROTO_FRAME_STUFFED_MAX];
static size_t s_rx_assembly_len = 0;
static bool s_rx_collecting = false;

// --- TX --------------------------------------------------------------------

// Caller must hold s_tx_lock. Not a blocking write: tud_cdc_write() queues
// into TinyUSB's own TX FIFO (CFG_TUD_CDC_TX_BUFSIZE, tusb_config.h -- sized
// well past one worst-case stuffed frame) and returns the count actually
// accepted; a short write (FIFO already backed up, or no host attached to
// drain it) is treated as failure rather than partially transmitting a
// frame the far end could misparse as a shorter, differently-shaped one --
// same "drop the whole frame, never a truncated one" rule
// uart_owner_send()'s own doc comment states for SaftyFW's link.
static bool usb_owner_cdc_write_locked(const uint8_t *data, size_t len)
{
    uint32_t written = tud_cdc_write(data, (uint32_t)len);
    if (written != len) {
        return false;
    }
    tud_cdc_write_flush();
    return true;
}

// Encodes+stuffs `frame` and transmits it under s_tx_lock. If
// `cache_task_id` is < USB_OWNER_ACK_CACHE_SLOTS, the stuffed bytes are also
// saved into that slot for a future DUPLICATE_REACK resend -- callers pass
// an out-of-range value (USB_OWNER_ACK_CACHE_SLOTS itself) for frames that
// are never resent this way (NACKs: BENCHPROTO.md sec 4 doesn't ask a NACK
// to survive a retry, the host's own retry of the original DATA frame will
// simply produce a fresh NACK).
static bool usb_owner_encode_and_transmit(const benchproto_frame_t *frame, uint8_t cache_task_id)
{
    uint8_t raw[BENCHPROTO_FRAME_RAW_MAX];
    benchproto_frame_status_t status;
    size_t raw_len = benchproto_frame_encode_raw(frame, raw, sizeof(raw), &status);
    if (raw_len == 0) {
        return false;
    }

    uint8_t stuffed[BENCHPROTO_FRAME_STUFFED_MAX];
    size_t stuffed_len = benchproto_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0) {
        return false;
    }

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(USB_OWNER_TX_LOCK_WAIT_MS)) != pdTRUE) {
        return false;
    }

    bool ok = usb_owner_cdc_write_locked(stuffed, stuffed_len);
    if (ok && cache_task_id < USB_OWNER_ACK_CACHE_SLOTS) {
        memcpy(s_ack_cache[cache_task_id].stuffed, stuffed, stuffed_len);
        s_ack_cache[cache_task_id].len = stuffed_len;
        s_ack_cache[cache_task_id].valid = true;
    }

    xSemaphoreGive(s_tx_lock);
    return ok;
}

static void usb_owner_resend_cached_ack(uint8_t task_id)
{
    if (task_id >= USB_OWNER_ACK_CACHE_SLOTS) {
        return;
    }
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(USB_OWNER_TX_LOCK_WAIT_MS)) != pdTRUE) {
        return;
    }
    if (s_ack_cache[task_id].valid) {
        (void)usb_owner_cdc_write_locked(s_ack_cache[task_id].stuffed, s_ack_cache[task_id].len);
    }
    xSemaphoreGive(s_tx_lock);
}

static void usb_owner_send_nack(const benchproto_frame_t *frame)
{
    benchproto_frame_t nack = {
        .msg_type = BENCHPROTO_MSG_NACK,
        .msg_index = frame->msg_index,
        .src_device = SIMFW_DEVICE_TARGET,
        .src_task = frame->dst_task,
        .dst_device = frame->src_device,
        .dst_task = frame->src_task,
        .length = 0,
        .payload = NULL,
    };
    (void)usb_owner_encode_and_transmit(&nack, USB_OWNER_ACK_CACHE_SLOTS);
}

bool usb_owner_send_reply(uint8_t responding_task_id, uint8_t dst_device, uint8_t dst_task, uint16_t msg_index,
                           const uint8_t *payload, uint8_t length)
{
    if (length > 0 && payload == NULL) {
        return false;
    }
    if ((size_t)length > BENCHPROTO_FRAME_MAX_PAYLOAD) {
        return false;
    }

    benchproto_frame_t ack = {
        .msg_type = BENCHPROTO_MSG_ACK,
        .msg_index = msg_index,
        .src_device = SIMFW_DEVICE_TARGET,
        .src_task = responding_task_id,
        .dst_device = dst_device,
        .dst_task = dst_task,
        .length = length,
        .payload = payload,
    };
    return usb_owner_encode_and_transmit(&ack, responding_task_id);
}

bool usb_owner_send_broadcast(uint8_t src_task, const uint8_t *payload, uint8_t length)
{
    if (length > 0 && payload == NULL) {
        return false;
    }
    if ((size_t)length > BENCHPROTO_FRAME_MAX_PAYLOAD) {
        return false;
    }

    benchproto_frame_t bcast = {
        .msg_type = BENCHPROTO_MSG_BROADCAST,
        .msg_index = 0, // BROADCAST is never retried/deduped/matched (BENCHPROTO.md sec 4)
        .src_device = SIMFW_DEVICE_TARGET,
        .src_task = src_task,
        .dst_device = SIMFW_DEVICE_HOST,
        .dst_task = 0, // no specific registered receiver task for unsolicited traffic
        .length = length,
        .payload = payload,
    };
    // Never cached (USB_OWNER_ACK_CACHE_SLOTS as the out-of-range sentinel,
    // same convention usb_owner_send_nack() uses above): a BROADCAST is
    // never resent for a DUPLICATE_REACK, so nothing needs saving.
    return usb_owner_encode_and_transmit(&bcast, USB_OWNER_ACK_CACHE_SLOTS);
}

bool usb_owner_register_task(uint8_t task_id)
{
    return benchproto_link_register_task(&s_link, task_id) == BENCHPROTO_LINK_OK;
}

// --- RX --------------------------------------------------------------------

static void usb_owner_handle_deliver(const benchproto_frame_t *frame, QueueHandle_t cmd_inbox)
{
    if (frame->msg_type != BENCHPROTO_MSG_DATA) {
        // A BROADCAST addressed to a registered task -- never ACKed, never
        // marked delivered (BENCHPROTO.md sec 4). Nothing on this side
        // consumes an inbound broadcast yet (cmd_task's inbox is
        // request/reply DATA only); dropped, same as an unrecognised
        // command byte elsewhere in this codebase.
        return;
    }
    if (cmd_inbox == NULL || frame->length == 0) {
        // length == 0 has no subcommand byte (PROTOCOL.md's "Reply
        // convention") to dispatch on -- untrusted/malformed input,
        // silently discarded like every other decode failure in this file.
        // The host's own retry (it never got an ACK) is what recovers, same
        // as a queue-full drop below.
        return;
    }

    cmd_task_request_t req;
    req.dst_task = frame->dst_task;
    req.src_device = frame->src_device;
    req.src_task = frame->src_task;
    req.msg_index = frame->msg_index;
    req.length = frame->length;
    memcpy(req.payload, frame->payload, frame->length);

    if (xQueueSend(cmd_inbox, &req, 0) != pdTRUE) {
        // Inbox full -- withhold the ACK so the host's retry gives cmd_task
        // time to drain (BENCHPROTO.md sec 4's backpressure rule). Do NOT
        // mark_delivered(): this message was never actually handed off.
        return;
    }

    // Handed off successfully -- mark_delivered() now, per
    // BENCHPROTO_LINK_ACTION_DELIVER's own contract ("only ... after a
    // caller confirms the hand-off succeeded"). The ACK itself (carrying
    // cmd_task's reply payload) is sent later, from usb_owner_send_reply(),
    // once cmd_task finishes dispatching -- not here.
    (void)benchproto_link_mark_delivered(&s_link, frame->dst_task, frame->src_device, frame->src_task,
                                          frame->msg_index);
}

static void usb_owner_handle_raw_frame(const uint8_t *stuffed, size_t stuffed_len, QueueHandle_t cmd_inbox)
{
    uint8_t unstuffed[BENCHPROTO_FRAME_STUFFED_MAX];
    benchproto_frame_status_t ustatus;
    size_t ulen = benchproto_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &ustatus);
    if (ulen == 0) {
        return; // unterminated escape or (shouldn't happen, same-size buffer) too small
    }

    benchproto_frame_t frame;
    if (benchproto_frame_decode(unstuffed, ulen, &frame) != BENCHPROTO_FRAME_OK) {
        return; // bad length/CRC/type -- untrusted wire input, discarded, not guessed at
    }

    if (frame.dst_device != SIMFW_DEVICE_TARGET) {
        return; // not addressed to this device
    }

    benchproto_link_action_t action = benchproto_link_on_frame(&s_link, NULL, &frame);
    switch (action) {
    case BENCHPROTO_LINK_ACTION_DELIVER:
        usb_owner_handle_deliver(&frame, cmd_inbox);
        break;
    case BENCHPROTO_LINK_ACTION_DUPLICATE_REACK:
        usb_owner_resend_cached_ack(frame.dst_task);
        break;
    case BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE:
        usb_owner_send_nack(&frame);
        break;
    case BENCHPROTO_LINK_ACTION_IGNORE:
    case BENCHPROTO_LINK_ACTION_ACK_MATCHED:
    case BENCHPROTO_LINK_ACTION_NACK_MATCHED:
    default:
        // ACK_MATCHED/NACK_MATCHED are unreachable with pending == NULL
        // (usb_owner never sends its own outstanding requests today --
        // see benchproto_link_on_frame()'s own doc comment) but are listed
        // explicitly rather than falling through a bare default, so a
        // future caller that DOES start passing a real `pending` here is
        // forced to revisit this switch instead of silently inheriting a
        // no-op case.
        break;
    }
}

// Resynchronises on 0x7E from any state (BENCHPROTO.md sec 2): a delimiter
// is always a frame boundary, whatever came before it. Bounded buffer, no
// allocation -- mirrors SaftyFW's link_task_rx_process_byte() exactly (see
// this file's header comment).
static void usb_owner_rx_process_byte(uint8_t b, QueueHandle_t cmd_inbox)
{
    if (b == BENCHPROTO_FRAME_DELIM) {
        if (s_rx_collecting && s_rx_assembly_len > 0) {
            usb_owner_handle_raw_frame(s_rx_assembly, s_rx_assembly_len, cmd_inbox);
        }
        s_rx_assembly_len = 0;
        s_rx_collecting = true; // this delimiter is simultaneously "end of previous" and "start of next"
        return;
    }

    if (!s_rx_collecting) {
        return; // noise before the first delimiter we've ever seen -- drop it
    }

    if (s_rx_assembly_len >= sizeof(s_rx_assembly)) {
        // Oversized run with no delimiter in sight -- drop what we have and
        // wait for the next 0x7E to resync, rather than growing without
        // bound or overwriting past the buffer.
        s_rx_collecting = false;
        s_rx_assembly_len = 0;
        return;
    }

    s_rx_assembly[s_rx_assembly_len++] = b;
}

// --- Task --------------------------------------------------------------------

static void usb_owner_task_fn(void *arg)
{
    (void)arg;

    // Must run after the scheduler has started (tusb_init() uses RTOS queue
    // API internally under CFG_TUSB_OS=OPT_OS_FREERTOS, tusb_config.h) --
    // this task body is exactly that context, never called from main()
    // directly.
    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(0, &dev_init);

    // cmd_task_start() (main.c's existing call order: usb_owner_start()
    // then cmd_task_start(), both before vTaskStartScheduler()) has already
    // run by the time this task body executes, so cmd_task_get_inbox()
    // should never actually return NULL here -- re-fetched every iteration
    // regardless, cheaply, rather than trusting that ordering silently.
    QueueHandle_t cmd_inbox = cmd_task_get_inbox();

    for (;;) {
        tud_task();

        if (cmd_inbox == NULL) {
            cmd_inbox = cmd_task_get_inbox();
        }

        while (tud_cdc_available()) {
            uint8_t buf[64];
            uint32_t n = tud_cdc_read(buf, sizeof(buf));
            for (uint32_t i = 0; i < n; i++) {
                usb_owner_rx_process_byte(buf[i], cmd_inbox);
            }
        }
    }
}

bool usb_owner_start(void)
{
    benchproto_link_init(&s_link, SIMFW_DEVICE_TARGET);
    memset(s_ack_cache, 0, sizeof(s_ack_cache));
    s_rx_assembly_len = 0;
    s_rx_collecting = false;

    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        return false;
    }

    BaseType_t ok = xTaskCreate(usb_owner_task_fn, "usb_owner", USB_OWNER_STACK_WORDS, NULL,
                                 SIMFW_PRIO_USB_OWNER, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_ELASTIC_PATH);
    return true;
}
