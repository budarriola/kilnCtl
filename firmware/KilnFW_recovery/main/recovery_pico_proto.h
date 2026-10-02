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
// overruns the FIFO. One ~260 B frame is ~11.3 ms on the wire at 230400 baud
// (10 bits/byte); the flash page program adds a few ms on top, so DATA frames
// are started this far apart. Tune from the bench; the gap-retransmit rounds
// absorb the occasional loss either way.
#define RPP_DATA_PACE_MS 15u
// Discovery: give up (and report "not responding") after this long.
#define RPP_DISCOVER_TIMEOUT_MS 12000u
#define RPP_DISCOVER_PROBE_MS 700u
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
//   2. Otherwise (bootloader, or an older application) the active slot cannot
//      be read. The operator may assert the target slot; absent that, the
//      bootloader's default for empty metadata is assumed (active = A, so
//      target = B).
typedef enum {
    RPP_TARGET_FROM_APP = 0,        // derived from Frame A's active slot
    RPP_TARGET_OPERATOR = 1,        // operator-asserted, not verifiable
    RPP_TARGET_ASSUMED_DEFAULT = 2, // bootloader default for empty metadata (B)
} rpp_target_source_t;

typedef struct {
    int target_slot; // RPP_SLOT_A / RPP_SLOT_B
    rpp_target_source_t source;
} rpp_target_t;

// `app_active_slot`: RPP_SLOT_A/B when Frame A reported it, else
// RPP_SLOT_UNKNOWN. `operator_slot`: RPP_SLOT_A/B when asserted, else UNKNOWN.
// A Frame A reading always wins over an operator assertion.
rpp_target_t rpp_resolve_target(int app_active_slot, int operator_slot);

// True when `image_slot` (from rpp_check_image) is the slot `t` says will be
// written. When it is not, the relay refuses before BEGIN so nothing is erased.
bool rpp_image_matches_target(int image_slot, rpp_target_t t);

// --- frame building (ESP -> Pico, stuffed wire bytes) ----------------------
// `cap` must be >= KILNLINK_FRAME_STUFFED_MAX. Returns wire bytes written, or
// 0 on failure. `msg_index` is the caller's monotonic counter; it is NOT reset
// between uploads (receiver-side dedup state outlives an upload).
size_t rpp_build_frame(uint16_t msg_index, const uint8_t *payload, size_t payload_len,
                       uint8_t *out, size_t cap);
// Payload packers: write into `payload` (>= KILNLINK_FRAME_MAX_PAYLOAD bytes),
// return the payload length (0 on a bad argument).
size_t rpp_pack_get_status(uint8_t *payload);
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

// --- gap-retransmit round logic (pure) --------------------------------------
typedef enum {
    RPP_GAP_WAIT = 0,       // nothing actionable yet: wait for the next status
    RPP_GAP_RETRANSMIT = 1, // resend the chunks the status lists
    RPP_GAP_SEND_END = 2,   // every chunk is in: send UPDATE_END
    RPP_GAP_FAIL = 3,       // no progress / too many reports: give up
} rpp_gap_action_t;

typedef struct {
    uint32_t last_received;
    uint32_t stalled;
    uint32_t batches;
} rpp_gap_tracker_t;

void rpp_gap_tracker_init(rpp_gap_tracker_t *t);
// Feed one fresh RECEIVING status. Complete means gap_count == 0 AND
// received_chunks >= total_chunks (a status can list no gaps while chunks past
// its cursor are still missing).
rpp_gap_action_t rpp_gap_next(rpp_gap_tracker_t *t, const rpp_status_t *st);

// Microseconds still to wait before the next DATA frame may start, 0 when the
// pacing deadline has passed.
uint32_t rpp_pace_wait_us(int64_t now_us, int64_t next_send_us);

// Number of 248-byte chunks for `len`.
uint32_t rpp_chunk_count(uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_PICO_PROTO_H
