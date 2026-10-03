// recovery_pico_proto.h -- pure (no ESP-IDF, no I/O) protocol helpers for the
// recovery image's Pico update relay: kilnlink frame building/parsing for the
// UPDATE_* commands, the UPDATE_STATUS parser, the image vector/slot/CRC
// validation, target-slot resolution and the timing constants. Host-tested by
// test_recovery_pico_proto.c; the FreeRTOS/UART side is recovery_pico.c.
//
// Wire facts (verified against the sources named, not guessed):
//  - Envelope: firmware/CommonFW kilnlink_frame.h (0x7E-delimited, byte-
//    stuffed, CRC16-CCITT-FALSE), linked via components/kilnlink.
//  - ESP -> Pico frames are BROADCAST type, src device 0 (ESP), src task 7,
//    dst device 2 (SAFETY), dst task 7 (hwAbstraction uart_protocol_send_
//    broadcast(), UART_TASK_ID_SAFETY); the reply direction is the reverse.
//  - Payload byte 0 is the command: UPDATE_BEGIN 0x10 (36 B header), DATA 0x11
//    (u32 LE offset + <= 248 B), END 0x12 (u32 LE crc32), ABORT 0x13, STATUS
//    0x14 (Pico -> ESP; 16 B header + u16 LE gap chunk indices). Layouts are
//    those of KilnFW's ota_pico_relay.c and SaftyFW's bootloader/
//    recovery_update.c, which are wire-compatible with the application
//    receiver.
#ifndef RECOVERY_PICO_PROTO_H
#define RECOVERY_PICO_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kilnlink/kilnlink_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- commands / addressing -------------------------------------------------
#define RPP_CMD_GET_STATUS 0x01u
#define RPP_CMD_ANNOUNCE_VERSION 0x0Fu
#define RPP_ANNOUNCE_PROTOCOL 16u
#define RPP_ANNOUNCE_MIN_COMPATIBLE 7u
#define RPP_CMD_UPDATE_BEGIN 0x10u
#define RPP_CMD_UPDATE_DATA 0x11u
#define RPP_CMD_UPDATE_END 0x12u
#define RPP_CMD_UPDATE_ABORT 0x13u
#define RPP_CMD_UPDATE_STATUS 0x14u
#define RPP_CMD_REBOOT 0x29u

#define RPP_DEVICE_ESP 0u
#define RPP_DEVICE_SAFETY 2u
#define RPP_TASK_SAFETY 7u

#define RPP_CHUNK_LEN 248u
#define RPP_BEGIN_HEADER_LEN 36u
#define RPP_VERSION_LEN 16u
#define RPP_STATUS_HEADER_LEN 16u
#define RPP_STATUS_MAX_GAPS 32u

// UPDATE_STATUS states (SaftyFW update_state_t; 8 and 9 are application-only).
typedef enum {
    RPP_STATE_IDLE = 0,
    RPP_STATE_REFUSED = 1,
    RPP_STATE_ERASING = 2,
    RPP_STATE_RECEIVING = 3,
    RPP_STATE_VERIFYING = 4,
    RPP_STATE_COMPLETE = 5,
    RPP_STATE_ABORTED = 6,
    RPP_STATE_FAILED = 7,
    RPP_STATE_REJECTED_SLOT_LINKAGE = 8,
    RPP_STATE_REFUSED_RUNNING_IMAGE_OVERLAP = 9,
} rpp_state_t;

// UPDATE_STATUS error bits.
#define RPP_ERR_RELAY_CLOSED (1u << 0)
#define RPP_ERR_TRIP_PENDING (1u << 1)
#define RPP_ERR_TOO_HOT (1u << 2)
#define RPP_ERR_HEADER_INVALID (1u << 3)
#define RPP_ERR_VERSION_INCOMPATIBLE (1u << 4)
#define RPP_ERR_RETRANSMIT_CAP (1u << 5)
#define RPP_ERR_CRC_MISMATCH (1u << 6)
#define RPP_ERR_INTERNAL (1u << 7)

// --- timing ---------------------------------------------------------------
// The bootloader receiver polls a 32-byte UART FIFO and programs flash with
// interrupts off, so a DATA frame arriving while it programs the previous one
// overruns the FIFO (SaftyFW bootloader/recovery_update.c). One ~260 B frame is
// ~11.3 ms on the wire at 230400 baud (10 bits/byte); the flash page program
// adds a few ms on top, so DATA frames are started this far apart. Tune from
// the bench; the gap-retransmit rounds absorb the occasional loss either way.
#define RPP_DATA_PACE_MS 15u
// The APPLICATION receiver has no FIFO problem but a different one: link_task
// hands every UPDATE_* frame to update_task through a 4-deep queue
// (UPDATE_TASK_QUEUE_DEPTH) that is drained once per wake, and a wake is
// UPDATE_TASK_POLL_MS = 100 ms plus whatever the drained frames cost (a flash
// page program each, a few ms; update_task.c:131-134, 1683-1697). A frame that
// finds the queue full is silently dropped (update_task.c:1610-1622). With N
// frames at spacing `pace` the most that can land inside one wake window of
// length L is floor(L / pace) + 1, which stays <= 4 only while L < 4 * pace.
// Taking L up to ~130 ms (100 ms poll + a worst-case drain of four flash
// writes) needs pace > 32.5 ms; 40 ms covers L up to 159 ms, a ~20% margin
// over that bound, and costs 137 s for a full 3436-chunk image.
#define RPP_DATA_PACE_APP_MS 40u
// Pace for the connected receiver (bootloader or application).
uint32_t rpp_data_pace_ms(bool bootloader);
// Discovery: give up (and report "not responding") after this long.
#define RPP_DISCOVER_TIMEOUT_MS 12000u
#define RPP_DISCOVER_PROBE_MS 700u
// After the application first answers, how long to wait for a V3 Frame A (its
// active slot) before falling back to the operator's slot.
#define RPP_APP_SLOT_WAIT_MS 3000u
// BEGIN reply (the precondition check is synchronous on the Pico).
#define RPP_BEGIN_REPLY_TIMEOUT_MS 5000u
// Erase of the whole 832 KB slot: the bootloader may be silent throughout.
// Sized to the worst case, same reasoning as KilnFW ota_pico_relay.c.
#define RPP_ERASE_TIMEOUT_MS 120000u
#define RPP_END_REPLY_TIMEOUT_MS 15000u
#define RPP_GAP_ROUND_WAIT_MS 2500u
// Consecutive gap reports with no increase in received_chunks before giving up.
#define RPP_MAX_RETRANSMIT_ROUNDS 12u
// Hard cap on gap reports handled in one upload (a lossy link still ends).
#define RPP_MAX_GAP_BATCHES 400u
// The relay aborts a running update when nobody has polled its status for this
// long (the browser polls every second, so the page is gone).
#define RPP_CLIENT_GONE_MS 90000u

// --- image / flash layout (mirrors SaftyFW bootloader/flash_layout.h) -----
#define RPP_SLOT_SIZE 0x000D0000u
#define RPP_XIP_BASE 0x10000000u
#define RPP_SLOT_A_OFFSET 0x00011000u
#define RPP_SLOT_B_OFFSET 0x000E1000u
#define RPP_SRAM_BASE 0x20000000u
#define RPP_SRAM_END 0x20042000u

#define RPP_SLOT_A 0
#define RPP_SLOT_B 1
#define RPP_SLOT_UNKNOWN (-1)

// XIP address of a slot's first byte (its vector table); 0 for a bad slot.
uint32_t rpp_slot_xip_base(int slot);

typedef enum {
    RPP_IMG_OK = 0,
    RPP_IMG_EMPTY,
    RPP_IMG_TOO_BIG,
    RPP_IMG_BAD_SP,       // initial SP outside RP2040 SRAM
    RPP_IMG_BAD_THUMB,    // reset vector without the Thumb bit
    RPP_IMG_BAD_RESET,    // reset vector inside neither slot window
    RPP_IMG_CRC_MISMATCH, // recomputed CRC32 differs from the claimed one
} rpp_image_result_t;

const char *rpp_image_result_str(rpp_image_result_t r);

// zlib-compatible CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF), the same
// value the browser's crc32() and the Pico's bootloader_crc32() produce.
uint32_t rpp_crc32(const uint8_t *data, size_t len);

// Validates length, vectors (SP in SRAM, reset vector with the Thumb bit and
// inside exactly one slot's XIP window) and the CRC32 over the whole image.
// On RPP_IMG_OK *out_slot is the slot the image is linked for (RPP_SLOT_A or
// RPP_SLOT_B). On a vector failure *out_slot is RPP_SLOT_UNKNOWN. The CRC is
// checked last, so the vector verdict wins for a wrong-slot file whose CRC is
// fine (the more useful message).
rpp_image_result_t rpp_check_image(const uint8_t *img, size_t len, uint32_t claimed_crc,
                                   int *out_slot);

// --- target-slot resolution ------------------------------------------------
// The receivers choose the slot themselves (the opposite of the Pico's
// metadata active slot; BEGIN's requested_slot is advisory) and UPDATE_STATUS
// does not name it, and the bootloader never checks linkage at END -- so the
// ESP side must decide BEFORE sending BEGIN whether the file is for the slot
// that will be written. Sources, in order of trust:
//   1. The Pico application answers GET_STATUS with Frame A V3 whose flags2
//      carries ACTIVE_SLOT_KNOWN/ACTIVE_SLOT_B: target = the other slot.
//   2. Otherwise (bootloader, or an application that did not report it) the
//      active slot cannot be read and the OPERATOR must name the target. That
//      choice is never verifiable, so everything it produces says "target
//      unverified".
//   3. With neither, the target is UNRESOLVED and the relay refuses: it never
//      assumes a slot (a wrong guess leaves the Pico unbootable until SWD).
typedef enum {
    RPP_TARGET_FROM_APP = 0,   // derived from Frame A's active slot
    RPP_TARGET_OPERATOR = 1,   // operator-asserted, not verifiable
    RPP_TARGET_UNRESOLVED = 2, // nothing known: refuse
} rpp_target_source_t;

typedef struct {
    int target_slot; // RPP_SLOT_A / RPP_SLOT_B, or RPP_SLOT_UNKNOWN when unresolved
    rpp_target_source_t source;
} rpp_target_t;

// `app_active_slot`: RPP_SLOT_A/B when Frame A reported it, else
// RPP_SLOT_UNKNOWN. `operator_slot`: RPP_SLOT_A/B when asserted, else UNKNOWN.
// A Frame A reading always wins over an operator assertion.
rpp_target_t rpp_resolve_target(int app_active_slot, int operator_slot);

// True when `image_slot` (from rpp_check_image) is the slot `t` says will be
// written. False for an unresolved target, so nothing is erased.
bool rpp_image_matches_target(int image_slot, rpp_target_t t);

// Operator-facing refusal text for a target that is unresolved or does not
// match the image. `bootloader` selects the wording (the bootloader can never
// report its slot). Returns false (out untouched) when there is nothing to
// refuse, i.e. the target matches the image.
bool rpp_describe_target_refusal(rpp_target_t t, int image_slot, bool bootloader, char *out,
                                 size_t cap);

// --- frame building (ESP -> Pico, stuffed wire bytes) ----------------------
// `cap` must be >= KILNLINK_FRAME_STUFFED_MAX. Returns wire bytes written, or
// 0 on failure. `msg_index` is the caller's monotonic counter; it is NOT reset
// between uploads (receiver-side dedup state outlives an upload).
size_t rpp_build_frame(uint16_t msg_index, const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t cap);
// Payload packers: write into `payload` (>= KILNLINK_FRAME_MAX_PAYLOAD bytes),
// return the payload length (0 on a bad argument).
size_t rpp_pack_get_status(uint8_t *payload);
// ESP -> Pico ANNOUNCE_VERSION (0x0F, kilnlink protocol 16, min compatible 7,
// dirty, empty commit/datetime strings). The application only reports Frame A
// V3 (and so its active slot) to a peer that announced; a bootloader ignores it.
size_t rpp_pack_announce(uint8_t *payload);
size_t rpp_pack_begin(uint8_t *payload, uint32_t length, uint32_t crc32, const char *version);
size_t rpp_pack_data(uint8_t *payload, uint32_t offset, const uint8_t *data, size_t len);
size_t rpp_pack_end(uint8_t *payload, uint32_t crc32);
size_t rpp_pack_abort(uint8_t *payload);
size_t rpp_pack_reboot(uint8_t *payload);

// --- receive: streaming deframer ------------------------------------------
typedef struct {
    uint8_t wire[KILNLINK_FRAME_STUFFED_MAX];
    uint8_t raw[KILNLINK_FRAME_STUFFED_MAX];
    size_t n;
    bool overflow;
    uint32_t frames_ok;
    uint32_t frames_bad;
} rpp_rx_t;

void rpp_rx_init(rpp_rx_t *rx);
// Feeds one byte. Returns true when a complete, CRC-valid frame from the Pico
// (BROADCAST, src device SAFETY, dst device ESP) is available in *out; its
// payload pointer stays valid until the next push.
bool rpp_rx_push(rpp_rx_t *rx, uint8_t byte, kilnlink_frame_t *out);

// --- UPDATE_STATUS / Frame A parsing ---------------------------------------
typedef struct {
    uint8_t state;
    uint8_t err;
    uint32_t bytes_received;
    uint32_t total_chunks;
    uint32_t received_chunks;
    uint8_t gap_count;
    uint16_t gaps[RPP_STATUS_MAX_GAPS];
    // Optional two-byte trailer after the gap list (bootloader only today): the
    // bootloader's own metadata active slot and the slot a live transfer is
    // writing. RPP_SLOT_A/B, or RPP_SLOT_UNKNOWN when absent (older image, the
    // application's status) or reported unknown (0xFF on the wire).
    int active_slot;
    int target_slot;
} rpp_status_t;

// Payload includes the command byte at [0]. False on a short/wrong payload.
// gap_count is clamped to RPP_STATUS_MAX_GAPS; a payload too short for its own
// gap list is rejected (never partially applied).
bool rpp_parse_status(const uint8_t *payload, size_t len, rpp_status_t *out);

// Frame A (GET_STATUS reply, command byte 0x01): RPP_SLOT_A/B when a V3 frame
// reports a known active slot, RPP_SLOT_UNKNOWN otherwise (including a payload
// that is not a Frame A or is too short to carry flags2).
int rpp_parse_frame_a_active_slot(const uint8_t *payload, size_t len);

// Comma-joined human text for UPDATE_STATUS err bits ("none" for 0).
void rpp_format_err_bits(uint8_t err, char *out, size_t cap);

// Operator-facing text for an UPDATE_STATUS state + err bits that ends or
// refuses an update (REFUSED, FAILED, ABORTED, the app-only rejections).
// Never says "clear the trip": recovery has no CLEAR_TRIP path.
void rpp_describe_state(uint8_t state, uint8_t err, char *out, size_t cap);

// --- BEGIN wait (pure) -------------------------------------------------------
// BEGIN is resent only when the Pico demonstrably never saw it: several IDLE
// beacons AND a minimum time since the last send. A slow erase is silent (not
// IDLE), so it is never restarted.
//
// Only the BOOTLOADER beacons IDLE while no transfer is active
// (recovery_update.c:435-437). The application sends no IDLE beacons at all
// (update_task.c:1279 returns while no transfer is active), so for it a lost
// BEGIN can never be detected and is NEVER resent: the wait just runs to
// RPP_ERASE_TIMEOUT_MS and fails closed (RPP_BEGIN_FAIL).
#define RPP_BEGIN_IDLE_BEACONS 4u
#define RPP_BEGIN_RESEND_MIN_MS 5000u
#define RPP_BEGIN_MAX_SENDS 3u

typedef enum {
    RPP_BEGIN_WAIT = 0,
    RPP_BEGIN_RESEND = 1,    // send BEGIN again
    RPP_BEGIN_RECEIVING = 2, // the Pico is ready for DATA
    RPP_BEGIN_ERASING = 3,   // informational: erase in progress, keep waiting
    RPP_BEGIN_FAIL = 4,      // erase window elapsed (or resends exhausted)
} rpp_begin_action_t;

typedef struct {
    uint32_t sends;
    uint32_t idle_beacons;
    int64_t last_send_ms;
    int64_t first_send_ms;
} rpp_begin_t;

// Records the first BEGIN send.
void rpp_begin_start(rpp_begin_t *b, int64_t now_ms);
// Feed a status (st != NULL) or a time tick (st == NULL).
rpp_begin_action_t rpp_begin_step(rpp_begin_t *b, const rpp_status_t *st, int64_t now_ms);

// --- finish phase: END, gap rounds, completion (pure) -----------------------
// Both receivers beacon UPDATE_STATUS ONLY while receiving, so once every chunk
// has arrived they go SILENT: a relay that waits for a "no gaps" status never
// gets one. The ESP must send END itself -- after the first pass, on any status
// with no gaps, and after a quiet round -- and let END's reply decide:
// RECEIVING + gaps = run a gap round and send END again; VERIFYING/COMPLETE =
// wait; FAILED/REJECTED = fail. Silence before END is never fatal by itself.
typedef enum {
    RPP_EV_STATUS = 0, // a fresh UPDATE_STATUS arrived
    RPP_EV_QUIET = 1,  // a full quiet round passed with no status
} rpp_fin_event_t;

typedef enum {
    RPP_FIN_WAIT = 0,       // nothing to do yet
    RPP_FIN_SEND_END = 1,   // send UPDATE_END, then call rpp_fin_end_sent()
    RPP_FIN_RETRANSMIT = 2, // resend the status's gap chunks, then send END
    RPP_FIN_DONE = 3,       // COMPLETE
    RPP_FIN_FAIL = 4,       // give up; f->why says why
    // The Pico may or may not have finished: END was sent (or VERIFYING seen)
    // and the final result never arrived (lost COMPLETE/FAILED, IDLE beacon,
    // silence, ENDs ignored). NOT a failure and NOT retryable blindly: the
    // caller must send no ABORT and tell the operator to power-cycle and check
    // the Pico's version. f->why carries the text.
    RPP_FIN_UNKNOWN = 5,
} rpp_fin_action_t;

#define RPP_MAX_END_SENDS 3u

typedef struct {
    uint32_t last_received;
    uint32_t stalled;
    uint32_t batches;
    uint32_t end_sends;   // unanswered END sends since the last progress
    bool end_outstanding; // an END was sent and has not been answered yet
    bool restart_timer;   // caller: restart the END reply timer, then clear this
    bool end_sent_once;   // an END has been sent at some point (it may have been accepted)
    bool verifying_seen;  // the Pico reported VERIFYING (END was certainly accepted)
    const char *why;      // static text, set with RPP_FIN_FAIL
} rpp_fin_t;

void rpp_fin_start(rpp_fin_t *f);
// The caller sent END (call right after every RPP_FIN_SEND_END / retransmit).
void rpp_fin_end_sent(rpp_fin_t *f);
// True when an UPDATE_STATUS proves the sender is a bootloader rather than a
// busy application: only an IDLE report does (the real application never sends
// IDLE, but does send ABORTED/FAILED/REFUSED with total_chunks == 0).
bool rpp_status_proves_bootloader(const rpp_status_t *st);
// The transfer is being stopped (operator Abort, browser gone) after END may
// have gone out. True means: report outcome UNKNOWN and send NO ABORT (the Pico
// may already have committed; ABORT is a no-op then, so sending it would only
// make the report claim "aborted" for a possibly-live image). `pico_terminal`
// is a stop caused by the Pico's own terminal state, which is a real answer.
// Note: after END, a UART write failure is still reported FAILED + ABORT (not
// unknown); that is practically unreachable with blocking uart_write_bytes.
bool rpp_fin_stop_is_unknown(const rpp_fin_t *f, bool pico_terminal);
// A fresh status that arrives together with a stop request wins over the stop
// when it is COMPLETE: the transfer is reported DONE, not UNKNOWN.
bool rpp_fin_status_beats_stop(const rpp_status_t *fresh);
// `st` is only read for RPP_EV_STATUS. `since_end_ms` is the time since the
// last END send (only read while an END is outstanding).
rpp_fin_action_t rpp_fin_step(rpp_fin_t *f, rpp_fin_event_t ev, const rpp_status_t *st,
                              uint32_t since_end_ms);

// Whole RTOS ticks to delay for `wait_us`, rounded UP (never short of the
// full pace); 0 when wait_us is 0.
uint32_t rpp_pace_delay_ticks(uint32_t wait_us, uint32_t tick_us);

// Microseconds still to wait before the next DATA frame may start, 0 when the
// pacing deadline has passed.
uint32_t rpp_pace_wait_us(int64_t now_us, int64_t next_send_us);

// Number of 248-byte chunks for `len`.
uint32_t rpp_chunk_count(uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_PICO_PROTO_H
