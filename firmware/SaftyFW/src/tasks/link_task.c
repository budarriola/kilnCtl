// link_task.c -- Phase 7/7b/8 (partial): kilnlink BROADCAST TX of the
// existing 23-byte status frame and SAFETY_CMD_FW_VERSION, RX handling of
// ANNOUNCE_VERSION and GET_FW_VERSION, and the receiver hardening
// (resync-on-0x7E, bounded buffers, no allocation) LINK_PROTOCOL.md section 3
// requires. Context-frame parsing (SAFETY_CMD_PUSH_CONTEXT), SET_FIRING_
// CEILING, CLEAR_TRIP, SET_CLOCK and SAFETY_CMD_DIAG are explicitly out of
// scope for this pass -- see docs/TODO.md Phase 7/8's remaining checkboxes.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): this file must
// never reference GPIO6 or the relay, by name, number or symbol -- not even
// in a comment intended as an example. tools/check_isolation.ps1 greps this
// file for exactly that. Nothing below touches relay_owner.h, board_pins.h's
// SAFTYFW_PIN_RELAY, or hardware/gpio.h's relay pin -- only UART1, via
// uart_owner.
//
// A direct consequence, worth stating plainly rather than leaving implicit:
// this file structurally CANNOT ask relay_owner what GPIO6 is actually doing
// -- not "chooses not to", cannot, because doing so would mean including or
// calling into relay_owner, which is exactly what the isolation check exists
// to forbid. The two status-frame bits that need that answer instead go
// through safety_core_get_output_status() -- safety_core.h is not link/uart
// shaped, safety_core already legitimately depends on relay_owner (it
// commands it), and this file never names the word this comment is
// otherwise avoiding. See safety_core.h's doc comment on that function for
// the exact bit semantics.
#include "link_task.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "pico/time.h"

#include "task_priorities.h"
#include "watchdog_task.h"

#include "uart_owner.h"
#include "link_frame.h"

#include "current_task.h"
#include "discrete_task.h"
#include "safety_core.h"
#include "snapshots.h"
#include "thermo_task.h"

#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_version.h"

#define LINK_TASK_STACK_WORDS      configMINIMAL_STACK_SIZE
// Bounded wait, not a blocking read: this task also owns the 500 ms TX
// cadence and must check in with watchdog_task, so it polls uart_owner's RX
// ring on a short period rather than blocking on a queue receive.
#define LINK_TASK_POLL_MS          100
#define LINK_STATUS_TX_PERIOD_MS   500

// Addressing (CommonFW/docs/LINK_PROTOCOL.md section 3, firmware/KilnFW/App/
// drivers/espInterfaces/uart_protocol.h and uart_task_ids.h). Mirrored here
// rather than included: SaftyFW does not, and must not, depend on KilnFW
// headers (CLAUDE.md/TODO.md -- the two firmwares are independently built),
// and these three values are part of the frozen compatibility-floor wire
// contract (LINK_PROTOCOL.md section 4), not implementation detail that
// could plausibly drift.
#define LINK_DEVICE_ESP     0u // UART_PROTO_DEVICE_ESP
#define LINK_DEVICE_SAFETY  2u // UART_PROTO_DEVICE_SAFETY
#define LINK_TASK_ID_SAFETY 7u // UART_TASK_ID_SAFETY

// Bounds for the RX frame assembler below. Sized the same as uart_owner's own
// TX ring (link_frame.h's largest payload is well under 128 bytes raw); an
// oversized or malformed run of bytes between delimiters just gets dropped
// and resynced on the next 0x7E, per LINK_PROTOCOL.md section 3.
#define LINK_RX_ASSEMBLY_MAX  128u
#define LINK_RX_POLL_BUF      64u

static TaskHandle_t s_task_handle = NULL;

// Written only by link_task_fn (or functions it calls, all running on this
// task); read by link_task_get_degraded_no_context() from any task. volatile
// single-word read/write is sufficient, same pattern discrete_task.h/
// relay_owner.h already use for their own cross-task flags.
static volatile bool s_degraded_no_context = false;

static uint16_t s_msg_index = 0;
static uint8_t s_boot_id = 0;

static uint8_t s_rx_assembly[LINK_RX_ASSEMBLY_MAX];
static size_t s_rx_assembly_len = 0;
static bool s_rx_collecting = false;

// --- TX ----------------------------------------------------------------

static void link_task_send_broadcast(const uint8_t *payload, uint8_t length)
{
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = s_msg_index++,
        .src_device = LINK_DEVICE_SAFETY,
        .src_task = LINK_TASK_ID_SAFETY,
        .dst_device = LINK_DEVICE_ESP,
        .dst_task = LINK_TASK_ID_SAFETY,
        .length = length,
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    if (raw_len == 0) {
        return; // encode failure -- shouldn't happen for a well-formed frame we built ourselves
    }

    uint8_t stuffed[KILNLINK_FRAME_STUFFED_MAX];
    size_t stuffed_len = kilnlink_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0) {
        return;
    }

    // uart_owner_send() is itself non-blocking and drops the WHOLE frame if
    // the TX ring has no room (its own counter tracks that) -- exactly
    // LINK_PROTOCOL.md section 2 rule 3. Nothing here retries or escalates.
    (void)uart_owner_send(stuffed, stuffed_len);
}

static void link_task_send_status(void)
{
    thermo_snapshot_t th;
    bool th_present = thermo_task_get_snapshot(&th);
    // th.tc_c/th.cj_c are already NaN whenever th.valid is false
    // (snapshots.h's contract); the ternaries below only cover the
    // th_present == false case (nothing published yet), where *out is
    // documented as zeroed rather than NaN.
    bool temp_valid = th_present && th.valid;
    float tc_c = temp_valid ? th.tc_c : NAN;
    float cj_c = temp_valid ? th.cj_c : NAN;
    uint8_t fault_bits = th_present ? th.fault_bits : 0;

    current_snapshot_t cur;
    current_task_get_snapshot(&cur);

    bool estop = discrete_task_estop_pressed();

    uint8_t payload[LINK_FRAME_STATUS_LEN];
    bool energized_bit = false;
    bool enabled_bit = false;
    safety_core_get_output_status(&energized_bit, &enabled_bit);
    link_frame_pack_status(payload, estop, energized_bit, enabled_bit, temp_valid, tc_c, cj_c,
                            fault_bits, cur.amps[0], cur.amps[1], cur.amps[2]);

    link_task_send_broadcast(payload, LINK_FRAME_STATUS_LEN);
}

static void link_task_send_fw_version(void)
{
    // build_info.h (git commit, dirty flag, build timestamp) is not
    // generated yet -- TODO.md Phase 8 item, not this pass. "Unknown must map
    // to dirty = 1" (LINK_PROTOCOL.md section 4) is honoured trivially: an
    // unknown commit is reported dirty, never falsely clean. config_version/
    // config_crc are 0 -- there is no config_store yet (Phase 9), and the
    // spec documents 0 as meaning exactly that: "running on compiled-in
    // defaults that were never commissioned."
    uint8_t payload[16];
    size_t len = link_frame_pack_fw_version(payload, sizeof(payload), KILNLINK_PROTOCOL_VERSION,
                                             KILNLINK_MIN_COMPATIBLE, /* dirty = */ 1, NULL, 0, NULL,
                                             0, s_boot_id, /* config_version = */ 0,
                                             /* config_crc = */ 0);
    if (len == 0) {
        return;
    }

    link_task_send_broadcast(payload, (uint8_t)len);
}

// --- RX ------------------------------------------------------------------

static void link_task_handle_announce_version(const kilnlink_frame_t *frame)
{
    // Offsets 1..2 = peer protocol version, 3..4 = peer min_compatible, both
    // u16 LE, fixed offset (LINK_PROTOCOL.md section 4: "read bytes 1-4
    // first"). Anything shorter is malformed/truncated -- ignored, not
    // guessed at.
    if (frame->length < 5) {
        return;
    }

    uint16_t peer_protocol = (uint16_t)(frame->payload[1] | ((uint16_t)frame->payload[2] << 8));
    uint16_t peer_min_compatible =
        (uint16_t)(frame->payload[3] | ((uint16_t)frame->payload[4] << 8));

    bool compatible = link_frame_versions_compatible(KILNLINK_PROTOCOL_VERSION,
                                                       KILNLINK_MIN_COMPATIBLE, peer_protocol,
                                                       peer_min_compatible);

    // LINK_PROTOCOL.md section 4, "What each side does about a mismatch":
    // the Pico enters DEGRADED_NO_CONTEXT and does NOT latch a trip. This is
    // the ONLY effect a version mismatch has from in here -- no relay/trip
    // call, by design (and this file could not make one anyway, see the
    // header comment).
    s_degraded_no_context = !compatible;
}

static void link_task_handle_raw_frame(const uint8_t *stuffed, size_t stuffed_len)
{
    uint8_t unstuffed[LINK_RX_ASSEMBLY_MAX];
    kilnlink_frame_status_t ustatus;
    size_t ulen = kilnlink_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &ustatus);
    if (ulen == 0) {
        return; // unterminated escape or (shouldn't happen, same-size buffer) too small
    }

    kilnlink_frame_t frame;
    if (kilnlink_frame_decode(unstuffed, ulen, &frame) != KILNLINK_FRAME_OK) {
        return; // bad length/CRC/type -- untrusted wire input, discarded, not guessed at
    }

    if (frame.msg_type != KILNLINK_MSG_BROADCAST || frame.length == 0) {
        return; // the Pico never participates in the ACK'd DATA/ACK/NACK transport
    }

    uint8_t cmd = frame.payload[0];
    switch (cmd) {
    case LINK_FRAME_ANNOUNCE_VERSION_CMD:
        link_task_handle_announce_version(&frame);
        break;
    case LINK_FRAME_FW_VERSION_CMD:
        // Same id as the request (SAFETY_CMD_GET_FW_VERSION), distinguished
        // by direction and length: the ESP's request is exactly 1 byte, no
        // arguments. Answer every copy seen, idempotently -- the ESP is the
        // side allowed to retry (LINK_PROTOCOL.md section 2).
        if (frame.length == 1) {
            link_task_send_fw_version();
        }
        break;
    default:
        // Everything else (context frames, CLEAR_TRIP, SET_FIRING_CEILING,
        // SET_CLOCK, ...) is genuinely out of scope this pass -- see this
        // file's header comment and TODO.md Phase 7's remaining checkboxes.
        // An unrecognised type is silently discarded, matching
        // LINK_PROTOCOL.md's own additive-compatibility principle: "a peer
        // that has never heard of it discards it."
        break;
    }
}

// Resynchronises on 0x7E from any state (LINK_PROTOCOL.md section 3): a
// delimiter is always a frame boundary, whatever came before it. Bounded
// buffer, no allocation. A break (a long run of non-delimiter noise, or
// nothing at all) just never completes a frame -- tolerated as "peer not up",
// not latched as an error.
static void link_task_rx_process_byte(uint8_t b)
{
    if (b == KILNLINK_FRAME_DELIM) {
        if (s_rx_collecting && s_rx_assembly_len > 0) {
            link_task_handle_raw_frame(s_rx_assembly, s_rx_assembly_len);
        }
        s_rx_assembly_len = 0;
        s_rx_collecting = true; // this delimiter is simultaneously "end of previous" and "start of next"
        return;
    }

    if (!s_rx_collecting) {
        return; // noise before the first delimiter we've ever seen -- drop it
    }

    if (s_rx_assembly_len >= LINK_RX_ASSEMBLY_MAX) {
        // Oversized run with no delimiter in sight -- drop what we have and
        // wait for the next 0x7E to resync, rather than growing without
        // bound or overwriting past the buffer.
        s_rx_collecting = false;
        s_rx_assembly_len = 0;
        return;
    }

    s_rx_assembly[s_rx_assembly_len++] = b;
}

// --- Task ------------------------------------------------------------------

static void link_task_fn(void *arg)
{
    (void)arg;

    // Boot push, unsolicited, before entering the steady loop
    // (LINK_PROTOCOL.md section 6, Frame C: "pushed unsolicited once at
    // boot" -- this is what tells the ESP "the safety processor just
    // restarted" without polling for it).
    link_task_send_fw_version();

    TickType_t last_status_tx = xTaskGetTickCount();

    for (;;) {
        uint8_t rx_buf[LINK_RX_POLL_BUF];
        size_t n = uart_owner_rx_read(rx_buf, sizeof(rx_buf));
        for (size_t i = 0; i < n; i++) {
            link_task_rx_process_byte(rx_buf[i]);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_tx) >= pdMS_TO_TICKS(LINK_STATUS_TX_PERIOD_MS)) {
            link_task_send_status();
            last_status_tx = now;
        }

        vTaskDelay(pdMS_TO_TICKS(LINK_TASK_POLL_MS));

        watchdog_task_checkin(WATCHDOG_CHECKIN_LINK_TASK);
    }
}

bool link_task_start(void)
{
    // Pseudo-random, latched once at boot. Nothing else in this build sources
    // an identity value (boot_reason.h only tracks trip-reason survival
    // across a watchdog reset, not a boot counter/id) -- time_us_64() has
    // been running since well before this call, so its low bits are a cheap,
    // adequate source of "looks different each boot." This is diagnostic
    // identity only (so the ESP can tell "the Pico just restarted" apart
    // from "same Pico, still running"), not a security or safety value, so
    // true entropy is not required.
    s_boot_id = (uint8_t)(time_us_64() ^ (time_us_64() >> 8));
    s_degraded_no_context = false;
    s_msg_index = 0;
    s_rx_assembly_len = 0;
    s_rx_collecting = false;

    BaseType_t ok = xTaskCreate(link_task_fn, "link_task", LINK_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_LINK_TASK, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_LINK_PATH);
    return true;
}

bool link_task_get_degraded_no_context(void)
{
    return s_degraded_no_context;
}
