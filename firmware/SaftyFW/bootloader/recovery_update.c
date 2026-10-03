// recovery_update.c -- see recovery_update.h.
#include "recovery_update.h"

#include <string.h>

#include "pico/time.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h" // XIP_BASE
#include "hardware/sync.h"
#include "hardware/uart.h"

#include "crc32.h"
#include "flash_layout.h"
#include "metadata.h"
#include "persist.h"

#include "image_header.h"    // src/update/
#include "received_ranges.h" // src/update/
#include "update_receiver.h" // src/update/

#include "update_task_slot_linkage.h" // src/tasks/ -- pure, no RTOS/SDK dependency

#include "kilnlink/kilnlink_frame.h"

// --- Wire command ids (src/tasks/link_frame.h's values -- redefined here,
// same convention that file itself uses, since this bare-metal target does
// not link src/tasks/link_frame.c/.h, which pulls in safety_guards.h/
// snapshots.h it has no business depending on). These four values are part
// of the frozen wire contract (LINK_PROTOCOL.md / UPDATE_PROTOCOL.md), not
// implementation detail that could drift. ---
#define RECOVERY_CMD_UPDATE_BEGIN  0x10u
#define RECOVERY_CMD_UPDATE_DATA   0x11u
#define RECOVERY_CMD_UPDATE_END    0x12u
#define RECOVERY_CMD_UPDATE_ABORT  0x13u
#define RECOVERY_CMD_UPDATE_STATUS 0x14u

// LINK_DEVICE_ESP/_SAFETY, LINK_TASK_ID_SAFETY -- src/tasks/link_task.c's own
// mirrored copy of CommonFW/docs/LINK_PROTOCOL.md section 3's addressing,
// mirrored here again for the same reason that file gives: SaftyFW must not
// depend on KilnFW headers, and this bare-metal target additionally must not
// depend on src/tasks/link_task.c/.h.
#define RECOVERY_DEVICE_ESP     0u
#define RECOVERY_DEVICE_SAFETY  2u
#define RECOVERY_TASK_ID_SAFETY 7u

// --- RX byte assembler (mirrors src/tasks/link_task.c's
// link_task_rx_process_byte()/s_rx_assembly, polled instead of RTOS-driven)
#define RECOVERY_RX_ASSEMBLY_MAX KILNLINK_FRAME_STUFFED_MAX

static uint8_t s_rx_assembly[RECOVERY_RX_ASSEMBLY_MAX];
static size_t s_rx_assembly_len = 0;
static bool s_rx_collecting = false;

// --- UPDATE_STATUS wire layout (byte-identical to update_task.c's own --
// see this file's header comment item 6) ---
#define STATUS_HEADER_LEN 16u
#define STATUS_MAX_GAPS   32u
// Two-byte slot trailer appended AFTER the gap list: [active_slot][target_slot],
// each 0/1 = slot A/B, STATUS_SLOT_UNKNOWN when not known. Both existing
// parsers (KilnFW safety_apply_update_status, KilnFW_recovery rpp_parse_status)
// require only header + gap_count*2 bytes and ignore anything after, so the
// trailer is backward compatible. active_slot is the bootloader's own metadata
// view (the slot main.c would boot), and the ESP prefers it over an operator
// guess. target_slot is the slot the transfer in progress is writing.
#define STATUS_SLOT_TRAILER_LEN 2u
#define STATUS_SLOT_UNKNOWN     0xFFu

// RP2040 architectural constants for the slot-linkage check, same values as
// update_task.c's UPDATE_TASK_SLOT_LINKAGE_* (fixed for every RP2040).
#define RECOVERY_LINKAGE_SRAM_BASE 0x20000000u
#define RECOVERY_LINKAGE_SRAM_END  0x20042000u
#define RECOVERY_LINKAGE_XIP_BASE  0x10000000u

typedef enum {
    R_STATE_IDLE = 0,
    R_STATE_REFUSED = 1,
    R_STATE_ERASING = 2,
    R_STATE_RECEIVING = 3,
    R_STATE_VERIFYING = 4,
    R_STATE_COMPLETE = 5,
    R_STATE_ABORTED = 6,
    R_STATE_FAILED = 7,
    // Same value update_task.c / UPDATE_PROTOCOL.md use for a CRC-good image
    // whose vector table does not belong to the target slot.
    R_STATE_REJECTED_SLOT_LINKAGE = 8,
} recovery_wire_state_t;

#define STATUS_ERR_RELAY_CLOSED         (1u << 0) // never set in recovery -- see header comment
#define STATUS_ERR_TRIP_PENDING         (1u << 1) // never set in recovery
#define STATUS_ERR_TOO_HOT              (1u << 2) // never set in recovery
#define STATUS_ERR_HEADER_INVALID       (1u << 3)
#define STATUS_ERR_VERSION_INCOMPATIBLE (1u << 4)
#define STATUS_ERR_RETRANSMIT_CAP       (1u << 5)
#define STATUS_ERR_CRC_MISMATCH         (1u << 6)
#define STATUS_ERR_INTERNAL             (1u << 7)

// --- Active-transfer state (single transfer at a time, same as
// update_task.c) ---
static bool s_transfer_active = false;
static uint8_t s_target_slot = BOOTLOADER_SLOT_A;
static uint32_t s_slot_flash_offset = 0;
static update_image_header_t s_header;
static update_received_ranges_t s_ranges;
static uint32_t s_gap_cursor = 0;
static uint32_t s_retransmit_round_count = 0;
static bool s_pass_had_gap = false;

// --- Little-endian helpers (same convention as update_task.c's own,
// small enough not to be worth a shared header for two call sites) ---

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

// --- Metadata read helper -------------------------------------------------

static size_t recovery_read_latest_metadata_or_default(bootloader_metadata_t *out_meta)
{
    const uint8_t *metadata_region = (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET);
    size_t latest = bootloader_metadata_find_latest(metadata_region, out_meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT) {
        memset(out_meta, 0, sizeof(*out_meta));
        out_meta->format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
        out_meta->active_slot = BOOTLOADER_SLOT_A;
        out_meta->slots[BOOTLOADER_SLOT_A].state = BOOTLOADER_SLOT_EMPTY;
        out_meta->slots[BOOTLOADER_SLOT_B].state = BOOTLOADER_SLOT_EMPTY;
    }
    return latest;
}

// --- Flash erase/program -- bare-metal equivalents of update_task.c's
// flash_safe_execute()-wrapped versions. No FreeRTOS SMP here, so plain
// save_and_disable_interrupts()/restore_interrupts() is the whole story
// (same reasoning as persist.c's bootloader_persist_metadata()) --------

// Erases the whole target slot in FLASH_BLOCK_SIZE (64K) units, one
// disable/erase/restore per block -- matches update_task_erase_slot()'s own
// "one bounded unit of work at a time" shape (that file's header comment),
// kept here even though this bare-metal image has no watchdog to starve,
// so the two erase paths stay easy to compare block-for-block.
static void recovery_erase_slot(uint32_t slot_offset)
{
    uint32_t offset = slot_offset;
    uint32_t remaining = BOOTLOADER_SLOT_FLASH_SIZE;

    while (remaining > 0u) {
        uint32_t chunk = (remaining < FLASH_BLOCK_SIZE) ? remaining : FLASH_BLOCK_SIZE;
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(offset, chunk);
        restore_interrupts(ints);
        offset += chunk;
        remaining -= chunk;
    }
}

// Read-modify-write program of one UPDATE_DATA chunk -- same technique as
// update_task_program_chunk() (see that file's header comment, "The
// 248-byte chunk / 256-byte flash page mismatch"): reads back the one or
// two full pages the chunk touches from the XIP alias (valid because the
// whole slot was erased up front), overlays this chunk's bytes, reprograms.
static bool recovery_program_chunk(uint32_t slot_flash_offset, uint32_t rel_offset,
                                    const uint8_t *data, size_t len)
{
    uint32_t abs_start = slot_flash_offset + rel_offset;
    uint32_t first_page = abs_start & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    uint32_t last_byte = abs_start + (uint32_t)len - 1u;
    uint32_t last_page = last_byte & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    uint32_t page_count = ((last_page - first_page) / FLASH_PAGE_SIZE) + 1u;

    if (page_count > 2u) {
        return false; // UPDATE_CHUNK_LEN (248) < 2*FLASH_PAGE_SIZE -- defensive, wire-derived len
    }

    uint8_t page_buf[2u * FLASH_PAGE_SIZE];
    const uint8_t *xip_src = (const uint8_t *)(XIP_BASE + first_page);
    memcpy(page_buf, xip_src, (size_t)page_count * FLASH_PAGE_SIZE);

    uint32_t buf_offset = abs_start - first_page;
    memcpy(page_buf + buf_offset, data, len);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(first_page, page_buf, (size_t)page_count * FLASH_PAGE_SIZE);
    restore_interrupts(ints);
    return true;
}

// --- TX --------------------------------------------------------------------

static void recovery_send_frame(const uint8_t *payload, uint8_t length)
{
    static uint16_t s_msg_index = 0;

    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = s_msg_index++,
        .src_device = RECOVERY_DEVICE_SAFETY,
        .src_task = RECOVERY_TASK_ID_SAFETY,
        .dst_device = RECOVERY_DEVICE_ESP,
        .dst_task = RECOVERY_TASK_ID_SAFETY,
        .length = length,
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    if (raw_len == 0u) {
        return;
    }

    uint8_t stuffed[KILNLINK_FRAME_STUFFED_MAX];
    size_t stuffed_len = kilnlink_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0u) {
        return;
    }

    for (size_t i = 0; i < stuffed_len; i++) {
        uart_putc_raw(uart1, (char)stuffed[i]);
    }
}

static void recovery_pack_and_send_status(uint8_t state, uint8_t err, const uint32_t *gaps,
                                           size_t gap_count)
{
    if (gap_count > STATUS_MAX_GAPS) {
        gap_count = STATUS_MAX_GAPS;
    }

    uint8_t buf[STATUS_HEADER_LEN + STATUS_MAX_GAPS * 2u + STATUS_SLOT_TRAILER_LEN];
    size_t len = 0;

    buf[len++] = (uint8_t)RECOVERY_CMD_UPDATE_STATUS;
    buf[len++] = state;
    buf[len++] = err;

    uint32_t bytes_received = 0, total_chunks = 0, received_chunks = 0;
    if (s_transfer_active) {
        total_chunks = s_ranges.total_chunks;
        received_chunks = update_received_ranges_count(&s_ranges);
        bytes_received = received_chunks * UPDATE_CHUNK_LEN;
    }
    put_u32_le(&buf[len], bytes_received); len += 4;
    put_u32_le(&buf[len], total_chunks); len += 4;
    put_u32_le(&buf[len], received_chunks); len += 4;

    buf[len++] = (uint8_t)gap_count;
    for (size_t i = 0; i < gap_count; i++) {
        put_u16_le(&buf[len], (uint16_t)gaps[i]);
        len += 2;
    }

    // Slot trailer. active_slot comes from the same metadata read main.c's
    // boot path does; target_slot only while a transfer is live.
    bootloader_metadata_t meta;
    size_t latest = recovery_read_latest_metadata_or_default(&meta);
    buf[len++] = (latest != BOOTLOADER_METADATA_NO_SLOT && meta.active_slot <= BOOTLOADER_SLOT_B)
                     ? (uint8_t)meta.active_slot
                     : (uint8_t)STATUS_SLOT_UNKNOWN;
    buf[len++] = s_transfer_active ? (uint8_t)s_target_slot : (uint8_t)STATUS_SLOT_UNKNOWN;

    recovery_send_frame(buf, (uint8_t)len);
}

static void recovery_send_status_now(uint8_t state, uint8_t err)
{
    recovery_pack_and_send_status(state, err, NULL, 0);
}

// --- Transfer teardown -------------------------------------------------------

static void recovery_revert_target_slot(void)
{
    if (!s_transfer_active) {
        return;
    }
    bootloader_metadata_t meta;
    size_t latest = recovery_read_latest_metadata_or_default(&meta);
    memset(&meta.slots[s_target_slot], 0, sizeof(meta.slots[s_target_slot]));
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_EMPTY;
    bootloader_persist_metadata(&meta, latest);
    s_transfer_active = false;
}

// --- UPDATE_BEGIN ------------------------------------------------------------

static void recovery_process_begin(const uint8_t *payload, uint8_t length)
{
    // No live safety state to gather in recovery mode -- see this file's
    // header comment item 2: an all-satisfied preconditions struct is the
    // only value that means "not applicable" under update_preconditions_t's
    // own contract, and it is safe here because GPIO6/the relay path has no
    // meaning in a bare-metal image that never started the application.
    update_preconditions_t precond = {
        .relay_open = true,
        .no_trip_pending = true,
        .temp_known_and_low = true,
    };

    update_image_header_t hdr;
    bool unpacked = false;
    if (length >= 1u) {
        unpacked = update_image_header_unpack(payload + 1u, (uint8_t)(length - 1u), &hdr);
    }
    if (!unpacked) {
        recovery_send_status_now(R_STATE_REFUSED, STATUS_ERR_HEADER_INVALID);
        return;
    }

    bootloader_metadata_t meta;
    size_t latest = recovery_read_latest_metadata_or_default(&meta);
    uint8_t active_slot = meta.active_slot;

    update_begin_decision_t decision =
        update_receiver_handle_begin(&hdr, &precond, active_slot, BOOTLOADER_SLOT_FLASH_SIZE);

    switch (decision.outcome) {
    case UPDATE_BEGIN_REFUSED_PRECONDITION:
        // Cannot happen given the all-satisfied struct above -- handled
        // anyway so this switch stays exhaustive and honest about the enum.
        recovery_send_status_now(R_STATE_REFUSED, STATUS_ERR_INTERNAL);
        return;
    case UPDATE_BEGIN_REFUSED_HEADER_INVALID:
        recovery_send_status_now(R_STATE_REFUSED, STATUS_ERR_HEADER_INVALID);
        return;
    case UPDATE_BEGIN_REFUSED_VERSION_INCOMPATIBLE:
        recovery_send_status_now(R_STATE_REFUSED, STATUS_ERR_VERSION_INCOMPATIBLE);
        return;
    case UPDATE_BEGIN_ACCEPTED:
        break;
    }

    s_target_slot = decision.target_slot;
    s_slot_flash_offset = (s_target_slot == BOOTLOADER_SLOT_A) ? BOOTLOADER_SLOT_A_FLASH_OFFSET
                                                                : BOOTLOADER_SLOT_B_FLASH_OFFSET;
    s_header = hdr;

    recovery_send_status_now(R_STATE_ERASING, 0);
    recovery_erase_slot(s_slot_flash_offset);

    memset(&meta.slots[s_target_slot], 0, sizeof(meta.slots[s_target_slot]));
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_STAGED;
    bootloader_persist_metadata(&meta, latest);

    update_received_ranges_reset(&s_ranges, hdr.length);
    s_gap_cursor = 0;
    s_retransmit_round_count = 0;
    s_pass_had_gap = false;
    s_transfer_active = true;

    recovery_send_status_now(R_STATE_RECEIVING, 0);
}

// --- UPDATE_DATA ---------------------------------------------------------

static void recovery_process_data(const uint8_t *payload, uint8_t length)
{
    if (!s_transfer_active || length < 1u + 4u + 1u) {
        return;
    }

    uint32_t offset = get_u32_le(&payload[1]);
    const uint8_t *data = &payload[5];
    size_t data_len = (size_t)length - 5u;
    if (data_len > UPDATE_CHUNK_LEN) {
        return;
    }

    if (!update_received_ranges_mark(&s_ranges, offset)) {
        return;
    }

    (void)recovery_program_chunk(s_slot_flash_offset, offset, data, data_len);
    // No sticky-error latch here (unlike update_task.c) -- recovery mode has
    // no periodic-status-independent side channel to report through beyond
    // what UPDATE_END's own read-back CRC already gates; a program failure
    // here still surfaces as a CRC mismatch at UPDATE_END.
}

// --- UPDATE_END ------------------------------------------------------------

static void recovery_process_end(const uint8_t *payload, uint8_t length)
{
    if (!s_transfer_active) {
        return;
    }
    if (length < 1u + 4u) {
        recovery_send_status_now(R_STATE_RECEIVING, STATUS_ERR_INTERNAL);
        return;
    }

    uint32_t end_crc = get_u32_le(&payload[1]);
    (void)end_crc; // logged-not-acted-on mismatch, per this file's header comment item 5 --
                    // no log_task in this bare-metal image to log through.

    if (!update_received_ranges_is_complete(&s_ranges)) {
        recovery_send_status_now(R_STATE_RECEIVING, 0);
        return;
    }

    recovery_send_status_now(R_STATE_VERIFYING, 0);

    const uint8_t *slot_data = (const uint8_t *)(XIP_BASE + s_slot_flash_offset);
    uint32_t actual_crc = bootloader_crc32(slot_data, s_header.length);

    if (actual_crc != s_header.crc32) {
        recovery_send_status_now(R_STATE_FAILED, STATUS_ERR_CRC_MISMATCH);
        recovery_revert_target_slot();
        return;
    }

    // Slot-linkage check, AFTER the CRC (a CRC-good image written for the
    // other slot is exactly what the CRC cannot catch: the bytes received are
    // the bytes sent) and BEFORE the slot is marked PENDING_VERIFY and made
    // active -- main.c jumps into it unconditionally, so a wrong-slot image
    // would otherwise fault at boot. The ESP-side check
    // (rpp_check_image) is a courtesy; this is the one that cannot be bypassed.
    if (s_header.length < 8u ||
        !update_task_slot_linkage_check(((const uint32_t *)slot_data)[0],
                                        ((const uint32_t *)slot_data)[1], s_header.length,
                                        s_slot_flash_offset, BOOTLOADER_SLOT_FLASH_SIZE,
                                        RECOVERY_LINKAGE_SRAM_BASE, RECOVERY_LINKAGE_SRAM_END,
                                        RECOVERY_LINKAGE_XIP_BASE)) {
        recovery_send_status_now(R_STATE_REJECTED_SLOT_LINKAGE, STATUS_ERR_CRC_MISMATCH);
        recovery_revert_target_slot();
        return;
    }

    bootloader_metadata_t meta;
    size_t latest = recovery_read_latest_metadata_or_default(&meta);
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_PENDING_VERIFY;
    meta.slots[s_target_slot].length = s_header.length;
    meta.slots[s_target_slot].crc32 = s_header.crc32;
    memcpy(meta.slots[s_target_slot].version, s_header.version, sizeof(meta.slots[s_target_slot].version));
    memset(meta.slots[s_target_slot].build_commit, 0, sizeof(meta.slots[s_target_slot].build_commit));
    meta.slots[s_target_slot].build_epoch = 0;

    meta.active_slot = s_target_slot;
    meta.boot_attempts = 0;

    bootloader_persist_metadata(&meta, latest);

    recovery_send_status_now(R_STATE_COMPLETE, 0);
    s_transfer_active = false;

    // Deliberately NOT rebooting/jumping here -- same choice as
    // update_task_process_end() (see this file's header comment item 7).
    // The operator/ESP resets the board; main.c's own boot path picks up
    // the newly-PENDING_VERIFY slot on the next boot.
}

// --- UPDATE_ABORT ------------------------------------------------------------

static void recovery_process_abort(void)
{
    recovery_revert_target_slot();
    recovery_send_status_now(R_STATE_ABORTED, 0);
}

// --- Periodic gap-report status (mirrors update_task_periodic_status()'s
// cursor-advance/round-count scheme exactly) ---------------------------------

static bool recovery_periodic_status(void)
{
    if (!s_transfer_active) {
        recovery_send_status_now(R_STATE_IDLE, 0);
        return true;
    }
    if (update_received_ranges_is_complete(&s_ranges)) {
        return true; // UPDATE_END will finish this transfer
    }

    uint32_t gaps[STATUS_MAX_GAPS];
    size_t gap_count =
        update_received_ranges_find_gaps(&s_ranges, s_gap_cursor, gaps, STATUS_MAX_GAPS);

    if (gap_count > 0) {
        s_pass_had_gap = true;
        uint32_t next_cursor = gaps[gap_count - 1] + 1u;
        if (next_cursor >= s_ranges.total_chunks) {
            if (s_pass_had_gap) {
                s_retransmit_round_count++;
            }
            s_pass_had_gap = false;
            s_gap_cursor = 0;
        } else {
            s_gap_cursor = next_cursor;
        }
    } else {
        s_gap_cursor = 0;
        s_pass_had_gap = false;
    }

    if (!update_retransmit_should_continue(s_retransmit_round_count)) {
        recovery_send_status_now(R_STATE_FAILED, STATUS_ERR_RETRANSMIT_CAP);
        recovery_revert_target_slot();
        return true;
    }

    recovery_pack_and_send_status(R_STATE_RECEIVING, 0, gaps, gap_count);
    return true;
}

// --- Dispatch ----------------------------------------------------------------

static void recovery_dispatch(const kilnlink_frame_t *frame)
{
    if (frame->msg_type != KILNLINK_MSG_BROADCAST || frame->length == 0) {
        return;
    }
    uint8_t cmd = frame->payload[0];
    switch (cmd) {
    case RECOVERY_CMD_UPDATE_BEGIN:
        recovery_process_begin(frame->payload, frame->length);
        break;
    case RECOVERY_CMD_UPDATE_DATA:
        recovery_process_data(frame->payload, frame->length);
        break;
    case RECOVERY_CMD_UPDATE_END:
        recovery_process_end(frame->payload, frame->length);
        break;
    case RECOVERY_CMD_UPDATE_ABORT:
        recovery_process_abort();
        break;
    default:
        // "minimal frame subset" (TODO.md item 10.4) -- everything else
        // (telemetry, context frames, ...) is out of scope for recovery
        // mode by design (docs/BOOTLOADER.md section 4), discarded exactly
        // like link_task.c discards an unrecognised command.
        break;
    }
}

static void recovery_handle_raw_frame(const uint8_t *stuffed, size_t stuffed_len)
{
    uint8_t unstuffed[RECOVERY_RX_ASSEMBLY_MAX];
    kilnlink_frame_status_t ustatus;
    size_t ulen = kilnlink_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &ustatus);
    if (ulen == 0) {
        return;
    }

    kilnlink_frame_t frame;
    if (kilnlink_frame_decode(unstuffed, ulen, &frame) != KILNLINK_FRAME_OK) {
        return;
    }

    recovery_dispatch(&frame);
}

// Resynchronises on 0x7E from any state -- byte-for-byte the same state
// machine as src/tasks/link_task.c's link_task_rx_process_byte(), polled
// here instead of driven by an RTOS RX path.
static void recovery_rx_process_byte(uint8_t b)
{
    if (b == KILNLINK_FRAME_DELIM) {
        if (s_rx_collecting && s_rx_assembly_len > 0) {
            recovery_handle_raw_frame(s_rx_assembly, s_rx_assembly_len);
        }
        s_rx_assembly_len = 0;
        s_rx_collecting = true;
        return;
    }

    if (!s_rx_collecting) {
        return;
    }

    if (s_rx_assembly_len >= RECOVERY_RX_ASSEMBLY_MAX) {
        s_rx_collecting = false;
        s_rx_assembly_len = 0;
        return;
    }

    s_rx_assembly[s_rx_assembly_len++] = b;
}

// --- Main loop -----------------------------------------------------------

#define RECOVERY_STATUS_PERIOD_US (1000u * 1000u) // 1s, matches the old beacon cadence

void recovery_update_run(void)
{
    s_transfer_active = false;
    s_rx_assembly_len = 0;
    s_rx_collecting = false;

    uint64_t last_status_us = time_us_64();

    for (;;) {
        while (uart_is_readable(uart1)) {
            uint8_t b = (uint8_t)uart_getc(uart1);
            recovery_rx_process_byte(b);
        }

        uint64_t now_us = time_us_64();
        if (now_us - last_status_us >= RECOVERY_STATUS_PERIOD_US) {
            recovery_periodic_status();
            last_status_us = now_us;
        }

        // No timeout out of recovery -- loop forever. "There is nothing
        // safe to time out into" (docs/BOOTLOADER.md section 4).
    }
}
