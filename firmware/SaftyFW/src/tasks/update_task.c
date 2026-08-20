// update_task.c -- see update_task.h. Phase 10's runtime half: wires the
// frozen, host-tested src/update/{image_header,received_ranges,
// update_receiver,confirm}.h decision logic to a real FreeRTOS task, real
// flash I/O, and real UPDATE_STATUS replies.
//
// --- Writing flash from a running FreeRTOS SMP application (RP2040) ---
//
// docs/ARCHITECTURE.md section 8 and docs/BOOTLOADER.md section 5's "Writing
// flash while running from flash" both state the rule: XIP is disabled chip-
// wide while flash_range_erase()/flash_range_program() are actually erasing/
// programming, so nothing on EITHER core may fetch an instruction from flash
// during that window, and any ISR that could fire during it must be RAM-
// resident. The vendored pico-sdk (PICO_SDK_PATH, checked this session:
// src/rp2_common/pico_flash/flash.c) shows exactly how flash_safe_execute()
// delivers that guarantee under FreeRTOS SMP: default_enter_safe_zone_
// timeout_ms() starts a temporary, maximum-priority task on the OTHER core
// whose only job is to call save_and_disable_interrupts() there and then
// spin-wait, and only once that handshake confirms the other core's IRQs are
// masked does it call save_and_disable_interrupts() on THIS core too
// ("we always want to disable IRQs on our core"). So for the whole duration
// of the callback flash_safe_execute() invokes, both cores have interrupts
// globally disabled -- no ISR, on either core, can fire at all, which is a
// stronger guarantee than "every ISR that touches flash is in RAM" and
// subsumes it. That is also why the erase/program callbacks below are
// ordinary flash-resident functions rather than __not_in_flash_func(): they
// call pico-sdk's flash_range_erase()/flash_range_program() directly, and
// hardware/flash.h's own header comment states those functions "make a
// static copy of the second stage bootloader in SRAM ... so that they can
// safely be called from flash-resident code" -- the SDK, not this file,
// owns the XIP-reentry trampoline. What this file DOES have to get right is
// everything flash_safe_execute() does NOT do for free: calling it once per
// bounded unit of work (a 64K block erase, a 256/512-byte page program, one
// metadata record write) rather than across the whole 832K slot in one
// call, and feeding the watchdog immediately before and after each such
// call -- see update_task_erase_slot() below.
//
// --- The 248-byte chunk / 256-byte flash page mismatch ---
//
// UPDATE_DATA carries UPDATE_CHUNK_LEN (248) bytes of image data per frame
// (CommonFW/docs/UPDATE_PROTOCOL.md section 4: "UART_PROTO_MAX_PAYLOAD is
// 253, so 248 bytes of image per frame after the 4-byte offset"). pico-sdk's
// flash_range_program() requires BOTH its offset and its count to be exact
// multiples of FLASH_PAGE_SIZE (256 bytes, hardware/flash.h's own doc
// comment: "Must be aligned to a 256-byte flash page" / "Must be a multiple
// of 256 bytes"). 248 does not divide 256, and successive chunks are 248
// bytes apart, so a chunk's [offset, offset+248) range is essentially never
// page-aligned and can span two pages. Writing a 248-byte chunk directly is
// therefore NOT valid on this hardware -- update_task_program_chunk() below
// buffers: it reads back the one or two full pages the chunk touches from
// the XIP-mapped alias (valid, since update_task_erase_slot() already erased
// the whole target slot before any UPDATE_DATA is accepted -- every
// not-yet-written byte reads back as 0xFF, and every already-landed
// neighbouring chunk's bytes read back as whatever was actually programmed),
// overlays this chunk's bytes at the correct sub-page offset, and reprograms
// the whole page range. This never asks flash to flip a bit from 0 back to
// 1 without an erase (illegal on NOR flash): the read-back-and-rewrite
// portion re-programs bits that are already 0 with the same value, and the
// still-erased portion goes from 1 to 0 for the first time, exactly once.
#include "update_task.h"

#include <math.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "pico/flash.h"
#include "pico/time.h"

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h" // XIP_BASE
#include "hardware/watchdog.h"        // watchdog_reboot() -- update_task_request_rollback()

#include "task_priorities.h"
#include "watchdog_task.h"

#include "current_task.h"
#include "link_frame.h"
#include "link_task.h"
#include "safety_core.h"
#include "snapshots.h"
#include "thermo_task.h"

#include "crc32.h"       // bootloader/ -- see CMakeLists.txt include path
#include "flash_layout.h" // bootloader/
#include "metadata.h"     // bootloader/

#include "confirm.h"         // src/update/
#include "image_header.h"    // src/update/
#include "received_ranges.h" // src/update/
#include "update_receiver.h" // src/update/

#include "kilnlink/kilnlink_frame.h" // KILNLINK_FRAME_MAX_PAYLOAD

#define UPDATE_TASK_STACK_WORDS (configMINIMAL_STACK_SIZE * 3) // page_buf below is 512 bytes
#define UPDATE_TASK_POLL_MS            100
#define UPDATE_STATUS_TX_PERIOD_MS     500  // matches link_task.c's own Frame A cadence
#define UPDATE_CONFIRM_TICK_PERIOD_MS  2000 // matches link_task.c's own Frame B cadence
#define UPDATE_TASK_QUEUE_DEPTH        4

// UPDATE_PROTOCOL.md section 1: "Measured temperature below a configured
// ceiling... Default 100 degrees C". No config_store exists yet (Phase 9,
// the same honest gap link_task.c's KILNLINK_DIAG_FLAG_CALIBRATION_MISSING
// already documents) -- this compile-time constant is what
// update_task_process_begin() enforces until a real per-installation value
// exists to source it from.
#define UPDATE_TASK_TEMP_CEILING_C 100.0f

// Proxy window for confirm.h's adc_sampling_ok: current_task.h has no direct
// "is sampling running" boolean (see current_task.h's own header comment:
// "the actual sampling/conversion math lives in current_sense.c"), so a
// current_snapshot_t timestamped within this many ms of "now" is treated as
// evidence current_task's SAFTYFW_PERIOD_CURRENT_TASK_MS = 50 ms loop is
// alive. Generous relative to that period so this is never the flaky part
// of the confirm gate.
#define UPDATE_TASK_ADC_FRESH_MS 2000u

// --- Wire layout: SAFETY_CMD_UPDATE_STATUS (0x14) -------------------------
// CommonFW/docs/UPDATE_PROTOCOL.md section 4 names this frame
// ("Pico->ESP: state, bytes received, last error") but never specifies a
// byte layout -- this is that layout, invented here since nothing else in
// this codebase defines it. Variable length: a fixed 16-byte header plus up
// to UPDATE_STATUS_MAX_GAPS 16-bit missing-chunk indices (received_ranges.h's
// "gap report cursor" scheme -- see update_task_periodic_status() below for
// how the cursor advances across calls). Comfortably inside the 253-byte
// kilnlink payload cap even at the maximum gap count (16 + 32*2 = 80 bytes).
//
// Offset  Size  Field
//      0     1  cmd (LINK_FRAME_UPDATE_STATUS_CMD)
//      1     1  state (update_task_wire_state_t)
//      2     1  last_error (bitmask, see UPDATE_STATUS_ERR_* below)
//      3     4  bytes_received (u32 LE) -- received_chunks * UPDATE_CHUNK_LEN,
//                 an approximation for the final (possibly short) chunk, per
//                 this file's own brief: "approx from
//                 update_received_ranges_count() * UPDATE_CHUNK_LEN"
//      7     4  total_chunks (u32 LE)
//     11     4  received_chunks (u32 LE)
//     15     1  gap_count (0..UPDATE_STATUS_MAX_GAPS, how many indices follow)
//     16   2*N  gap chunk indices (u16 LE each)
#define UPDATE_STATUS_HEADER_LEN 16u
#define UPDATE_STATUS_MAX_GAPS   32u

typedef enum {
    UPDATE_TASK_STATE_IDLE = 0,
    UPDATE_TASK_STATE_REFUSED = 1,
    UPDATE_TASK_STATE_ERASING = 2,
    UPDATE_TASK_STATE_RECEIVING = 3,
    UPDATE_TASK_STATE_VERIFYING = 4,
    UPDATE_TASK_STATE_COMPLETE = 5,
    UPDATE_TASK_STATE_ABORTED = 6,
    UPDATE_TASK_STATE_FAILED = 7,
} update_task_wire_state_t;

// last_error bits. The first three mirror update_receiver.h's
// update_precondition_flag_t bit-for-bit (see update_task_precond_to_wire()
// below) so a caller that already knows that enum recognises them; the rest
// are this frame's own additions for outcomes update_precondition_flag_t has
// no bit for.
#define UPDATE_STATUS_ERR_RELAY_CLOSED         (1u << 0)
#define UPDATE_STATUS_ERR_TRIP_PENDING         (1u << 1)
#define UPDATE_STATUS_ERR_TOO_HOT              (1u << 2)
#define UPDATE_STATUS_ERR_HEADER_INVALID       (1u << 3)
#define UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE (1u << 4)
#define UPDATE_STATUS_ERR_RETRANSMIT_CAP       (1u << 5)
#define UPDATE_STATUS_ERR_CRC_MISMATCH         (1u << 6)
#define UPDATE_STATUS_ERR_INTERNAL             (1u << 7) // erase/program/metadata-write failure, malformed frame

// --- link_task <-> update_task queue ---------------------------------------

typedef struct {
    uint8_t length;                             // frame->length, cmd byte included
    uint8_t payload[KILNLINK_FRAME_MAX_PAYLOAD]; // frame->payload, cmd byte at [0]
} update_task_msg_t;

static QueueHandle_t s_rx_queue = NULL;
static TaskHandle_t s_task_handle = NULL;

// --- Active-transfer state (single transfer at a time, matches
// UPDATE_PROTOCOL.md section 4's "one update at a time") ------------------
static bool s_transfer_active = false;
static uint8_t s_target_slot = BOOTLOADER_SLOT_A;
static uint32_t s_slot_flash_offset = 0;
static update_image_header_t s_header;
static update_received_ranges_t s_ranges;
static uint32_t s_gap_cursor = 0;
static uint32_t s_retransmit_round_count = 0;
static bool s_pass_had_gap = false;
static uint8_t s_sticky_error = 0; // latched, non-fatal per-chunk errors (e.g. a program failure); cleared on a new transfer

// --- Confirmation-gate state (PENDING_VERIFY -> VALID, item 10.8) --------
static bool s_confirm_pending = false;
static uint8_t s_own_slot = BOOTLOADER_SLOT_A;

// --- Little-endian helpers (this module's own, not shared with
// link_frame.c -- small enough not to be worth a shared header for two
// call sites) ---------------------------------------------------------------

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

// --- Metadata read/persist helpers ------------------------------------------
//
// Shares bootloader/metadata.c's frozen, host-tested pack/unpack/scan
// functions (compiled into this application build -- see CMakeLists.txt --
// NOT a second implementation) but not bootloader/main.c's persist_metadata()
// itself: that function uses save_and_disable_interrupts()/
// restore_interrupts(), correct for the bootloader's single-core bare-metal
// world but not this FreeRTOS SMP application, which needs
// flash_safe_execute()'s multicore lockout instead (see this file's header
// comment). The read-latest / next-write-slot / erase-if-wrapped / pack /
// flash_range_program() SHAPE is deliberately the same as main.c's, so the
// two are easy to compare side by side.

static size_t update_task_read_latest_metadata(bootloader_metadata_t *out_meta)
{
    const uint8_t *metadata_region = (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET);
    return bootloader_metadata_find_latest(metadata_region, out_meta);
}

// Same as above, but fills `*out_meta` with a fresh, empty-slots default
// record (format_version set, both slots EMPTY, active_slot A, seq/
// boot_attempts 0) instead of leaving it untouched when the metadata sector
// has never held a valid record -- lets every caller below treat "never
// written" and "read a real record" the same way (mutate one field, persist)
// rather than special-casing bootstrap. Returns the same latest_slot_index
// convention as bootloader_metadata_find_latest() (BOOTLOADER_METADATA_NO_SLOT
// on the bootstrap path, which update_task_persist_metadata() already knows
// means "write to slot 0, no erase needed" -- see
// bootloader_metadata_next_write_slot()/_next_write_needs_erase()'s own doc
// comments).
static size_t update_task_read_latest_metadata_or_default(bootloader_metadata_t *out_meta)
{
    size_t latest = update_task_read_latest_metadata(out_meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT) {
        memset(out_meta, 0, sizeof(*out_meta));
        out_meta->format_version = BOOTLOADER_METADATA_FORMAT_VERSION;
        out_meta->active_slot = BOOTLOADER_SLOT_A;
        out_meta->slots[BOOTLOADER_SLOT_A].state = BOOTLOADER_SLOT_EMPTY;
        out_meta->slots[BOOTLOADER_SLOT_B].state = BOOTLOADER_SLOT_EMPTY;
    }
    return latest;
}

typedef struct {
    size_t next_write_slot;
    bool needs_erase;
    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
} update_metadata_write_args_t;

static void update_metadata_write_cb(void *param)
{
    update_metadata_write_args_t *a = (update_metadata_write_args_t *)param;
    if (a->needs_erase) {
        flash_range_erase(BOOTLOADER_METADATA_FLASH_OFFSET, BOOTLOADER_METADATA_FLASH_SIZE);
    }
    flash_range_program(BOOTLOADER_METADATA_FLASH_OFFSET +
                             (uint32_t)a->next_write_slot * BOOTLOADER_METADATA_RECORD_LEN,
                         a->record, BOOTLOADER_METADATA_RECORD_LEN);
}

// Mirrors bootloader/main.c's persist_metadata(meta, latest_slot) exactly in
// shape (same seq-increment-once, next-write-slot, erase-if-wrapped, pack
// steps) but flash_safe_execute()-wrapped instead of a bare interrupt
// critical section, with watchdog checkins immediately before and after --
// this is a single ~256-byte-page write, nowhere near the erase's
// multi-hundred-millisecond cost, but the discipline is the same either way.
static bool update_task_persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index)
{
    meta->seq = meta->seq + 1u;

    update_metadata_write_args_t args;
    args.next_write_slot = bootloader_metadata_next_write_slot(latest_slot_index);
    args.needs_erase = bootloader_metadata_next_write_needs_erase(latest_slot_index);
    bootloader_metadata_pack(meta, args.record);

    watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
    int rc = flash_safe_execute(update_metadata_write_cb, &args, 1000u);
    watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
    return rc == PICO_OK;
}

// --- Flash erase (UPDATE_BEGIN acceptance) ---------------------------------

typedef struct {
    uint32_t offset;
    size_t count;
} update_erase_args_t;

static void update_erase_cb(void *param)
{
    update_erase_args_t *a = (update_erase_args_t *)param;
    flash_range_erase(a->offset, a->count);
}

// Erases the whole BOOTLOADER_SLOT_FLASH_SIZE (832K) target slot in
// FLASH_BLOCK_SIZE (64K) units -- one flash_safe_execute() call, and one
// watchdog checkin before AND after, per block (this file's header comment
// / this task's own brief: "the erasing task must check in with
// watchdog_task_checkin() immediately before and after each erase call, not
// rely on checking in during it -- nothing CAN run during the actual
// erase"). A single flash_safe_execute() call covering the whole 832K slot
// would halt both cores for on the order of a couple of seconds (13 blocks
// at "hundreds of milliseconds" each, docs/BOOTLOADER.md section 5) -- well
// past the 1 s hardware watchdog timeout with no chance for anything to feed
// it in between, so this deliberately does NOT do that; block-at-a-time is
// what BOOTLOADER.md section 5's still-open "erase up front or block-by-
// block" question resolves to here, chosen specifically so the watchdog
// stays fed across the whole operation. BOOTLOADER_SLOT_FLASH_SIZE
// (0xD0000, 851968 bytes) is exactly 13 * FLASH_BLOCK_SIZE with no
// remainder (flash_layout.h) -- both slots were sized block-aligned from the
// start, so no partial-block tail case exists here to get wrong.
static bool update_task_erase_slot(uint32_t slot_offset)
{
    uint32_t offset = slot_offset;
    uint32_t remaining = BOOTLOADER_SLOT_FLASH_SIZE;

    while (remaining > 0u) {
        uint32_t chunk = (remaining < FLASH_BLOCK_SIZE) ? remaining : FLASH_BLOCK_SIZE;

        watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
        update_erase_args_t args = { .offset = offset, .count = chunk };
        int rc = flash_safe_execute(update_erase_cb, &args, 2000u);
        watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);

        if (rc != PICO_OK) {
            return false;
        }

        offset += chunk;
        remaining -= chunk;
    }

    return true;
}

// --- Flash program (UPDATE_DATA) -------------------------------------------

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t count;
} update_program_args_t;

static void update_program_cb(void *param)
{
    update_program_args_t *a = (update_program_args_t *)param;
    flash_range_program(a->offset, a->data, a->count);
}

// See this file's header comment ("The 248-byte chunk / 256-byte flash page
// mismatch") for the read-modify-write reasoning. `rel_offset`/`len` are
// slot-relative and already validated by update_received_ranges_mark()
// before this is called.
static bool update_task_program_chunk(uint32_t slot_flash_offset, uint32_t rel_offset,
                                       const uint8_t *data, size_t len)
{
    uint32_t abs_start = slot_flash_offset + rel_offset;
    uint32_t first_page = abs_start & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    uint32_t last_byte = abs_start + (uint32_t)len - 1u;
    uint32_t last_page = last_byte & ~(uint32_t)(FLASH_PAGE_SIZE - 1u);
    uint32_t page_count = ((last_page - first_page) / FLASH_PAGE_SIZE) + 1u;

    // UPDATE_CHUNK_LEN (248) < 2 * FLASH_PAGE_SIZE (512), so a chunk can
    // never legitimately span more than two pages -- defensive check kept
    // anyway since `len` ultimately comes off the wire.
    if (page_count > 2u) {
        return false;
    }

    uint8_t page_buf[2u * FLASH_PAGE_SIZE];
    const uint8_t *xip_src = (const uint8_t *)(XIP_BASE + first_page);
    memcpy(page_buf, xip_src, (size_t)page_count * FLASH_PAGE_SIZE);

    uint32_t buf_offset = abs_start - first_page;
    memcpy(page_buf + buf_offset, data, len);

    update_program_args_t args = {
        .offset = first_page,
        .data = page_buf,
        .count = (size_t)page_count * FLASH_PAGE_SIZE,
    };
    int rc = flash_safe_execute(update_program_cb, &args, 1000u);
    return rc == PICO_OK;
}

// --- UPDATE_STATUS send -----------------------------------------------------

static void update_task_pack_and_send(uint8_t state, uint8_t err, const uint32_t *gaps,
                                       size_t gap_count)
{
    if (gap_count > UPDATE_STATUS_MAX_GAPS) {
        gap_count = UPDATE_STATUS_MAX_GAPS; // defensive clamp -- callers already cap this
    }

    uint8_t buf[UPDATE_STATUS_HEADER_LEN + UPDATE_STATUS_MAX_GAPS * 2u];
    size_t len = 0;

    buf[len++] = LINK_FRAME_UPDATE_STATUS_CMD;
    buf[len++] = state;
    buf[len++] = (uint8_t)(err | s_sticky_error);

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

    (void)link_task_send_safety(buf, (uint8_t)len);
}

static void update_task_send_status_now(uint8_t state, uint8_t err)
{
    update_task_pack_and_send(state, err, NULL, 0);
}

// One bit per unmet precondition, update_precondition_flag_t -> this file's
// UPDATE_STATUS_ERR_* bits. The first three are numerically identical today
// (both enums were designed bit-for-bit the same way), but this mapping is
// written out explicitly rather than relying on that coincidence, so a
// future change to either enum's numbering cannot silently desync the wire
// frame from update_receiver.h's own values.
static uint8_t update_task_precond_to_wire(uint8_t precondition_flags)
{
    uint8_t out = 0;
    if (precondition_flags & UPDATE_PRECOND_RELAY_CLOSED) {
        out |= UPDATE_STATUS_ERR_RELAY_CLOSED;
    }
    if (precondition_flags & UPDATE_PRECOND_TRIP_PENDING) {
        out |= UPDATE_STATUS_ERR_TRIP_PENDING;
    }
    if (precondition_flags & UPDATE_PRECOND_TOO_HOT) {
        out |= UPDATE_STATUS_ERR_TOO_HOT;
    }
    return out;
}

// --- Transfer teardown -------------------------------------------------------

// Reverts the currently-staged target slot's metadata to EMPTY and persists
// it -- shared by explicit UPDATE_ABORT handling and the internal
// retransmit-round-cap abort path. No-op (does not touch flash) if no
// transfer is currently active.
static void update_task_revert_target_slot(void)
{
    if (!s_transfer_active) {
        return;
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata_or_default(&meta);
    memset(&meta.slots[s_target_slot], 0, sizeof(meta.slots[s_target_slot]));
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_EMPTY;
    (void)update_task_persist_metadata(&meta, latest); // best-effort -- transfer is being torn down regardless

    s_transfer_active = false;
}

// --- UPDATE_BEGIN ------------------------------------------------------------

static void update_task_gather_preconditions(update_preconditions_t *out)
{
    bool relay_energized = false, heating_enabled = false;
    safety_core_get_output_status(&relay_energized, &heating_enabled);
    out->relay_open = !relay_energized;

    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    bool warn_active = false;
    uint8_t diag_state = 0;
    safety_core_get_diag_status(&trip_reason, &warn_active, &diag_state);
    out->no_trip_pending = (trip_reason == SAFETY_TRIP_NONE);

    thermo_snapshot_t th;
    bool th_present = thermo_task_get_snapshot(&th);
    bool temp_valid = th_present && th.valid && !isnan(th.tc_c);
    out->temp_known_and_low = temp_valid && (th.tc_c < UPDATE_TASK_TEMP_CEILING_C);
}

static void update_task_process_begin(const uint8_t *payload, uint8_t length)
{
    update_preconditions_t precond;
    update_task_gather_preconditions(&precond);

    update_image_header_t hdr;
    bool unpacked = false;
    if (length >= 1u) {
        unpacked = update_image_header_unpack(payload + 1u, (uint8_t)(length - 1u), &hdr);
    }

    if (!unpacked) {
        // Cannot call update_receiver_handle_begin() without a decoded
        // header -- update_image_header_unpack() leaves `hdr` completely
        // untouched on failure (its own doc comment), so there is nothing
        // safe to pass in. Preserve update_receiver_handle_begin()'s own
        // documented check order anyway (preconditions before the header is
        // even inspected): a kiln that is not idle refuses regardless of
        // whether the (unreadable) image is well-formed.
        uint8_t precond_flags = update_preconditions_check(&precond);
        if (precond_flags != 0) {
            update_task_send_status_now(UPDATE_TASK_STATE_REFUSED,
                                         update_task_precond_to_wire(precond_flags));
        } else {
            update_task_send_status_now(UPDATE_TASK_STATE_REFUSED, UPDATE_STATUS_ERR_HEADER_INVALID);
        }
        return;
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata_or_default(&meta);
    uint8_t active_slot = meta.active_slot;

    update_begin_decision_t decision =
        update_receiver_handle_begin(&hdr, &precond, active_slot, BOOTLOADER_SLOT_FLASH_SIZE);

    switch (decision.outcome) {
    case UPDATE_BEGIN_REFUSED_PRECONDITION:
        update_task_send_status_now(UPDATE_TASK_STATE_REFUSED,
                                     update_task_precond_to_wire(decision.precondition_flags));
        return;
    case UPDATE_BEGIN_REFUSED_HEADER_INVALID:
        update_task_send_status_now(UPDATE_TASK_STATE_REFUSED, UPDATE_STATUS_ERR_HEADER_INVALID);
        return;
    case UPDATE_BEGIN_REFUSED_VERSION_INCOMPATIBLE:
        update_task_send_status_now(UPDATE_TASK_STATE_REFUSED, UPDATE_STATUS_ERR_VERSION_INCOMPATIBLE);
        return;
    case UPDATE_BEGIN_ACCEPTED:
        break;
    }

    // decision.requested_slot_mismatch: worth logging (this codebase has no
    // log_task call site wired up in this file yet -- see TODO.md's note on
    // this pass) but not a refusal reason, per image_header.h's own header
    // comment: the Pico's own choice always wins.
    (void)decision.requested_slot_mismatch;

    s_target_slot = decision.target_slot;
    s_slot_flash_offset = (s_target_slot == BOOTLOADER_SLOT_A) ? BOOTLOADER_SLOT_A_FLASH_OFFSET
                                                                : BOOTLOADER_SLOT_B_FLASH_OFFSET;
    s_header = hdr;
    s_sticky_error = 0;

    update_task_send_status_now(UPDATE_TASK_STATE_ERASING, 0);

    if (!update_task_erase_slot(s_slot_flash_offset)) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_INTERNAL);
        return;
    }

    // Mark the target slot STAGED (erased, not yet holding a complete image)
    // before accepting any UPDATE_DATA -- this is what gives UPDATE_ABORT
    // something concrete to revert (EMPTY) if the transfer never completes,
    // and what a human reading the metadata log over SWD would want to see
    // mid-transfer rather than a stale record from whatever this slot held
    // before.
    memset(&meta.slots[s_target_slot], 0, sizeof(meta.slots[s_target_slot]));
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_STAGED;
    if (!update_task_persist_metadata(&meta, latest)) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_INTERNAL);
        return;
    }

    update_received_ranges_reset(&s_ranges, hdr.length);
    s_gap_cursor = 0;
    s_retransmit_round_count = 0;
    s_pass_had_gap = false;
    s_transfer_active = true;

    update_task_send_status_now(UPDATE_TASK_STATE_RECEIVING, 0);
}

// --- UPDATE_DATA ---------------------------------------------------------

static void update_task_process_data(const uint8_t *payload, uint8_t length)
{
    if (!s_transfer_active) {
        return; // stray/late frame from an aborted or completed prior transfer -- discard silently
    }
    if (length < 1u + 4u + 1u) {
        return; // too short to carry even one byte of image data -- malformed, discard
    }

    uint32_t offset = get_u32_le(&payload[1]);
    const uint8_t *data = &payload[5];
    size_t data_len = (size_t)length - 5u;
    if (data_len > UPDATE_CHUNK_LEN) {
        return; // cannot legitimately come from a correctly-built sender -- discard
    }

    // Bounds/duplicate-tolerant tracking lives in received_ranges.h --
    // false means `offset` is not an exact multiple of UPDATE_CHUNK_LEN or
    // its chunk index is outside this transfer's total_chunks (that
    // module's own doc comment), exactly the "stray frame from a different
    // transfer" defense this function's job description calls for.
    if (!update_received_ranges_mark(&s_ranges, offset)) {
        return;
    }

    if (!update_task_program_chunk(s_slot_flash_offset, offset, data, data_len)) {
        // Rare (flash_safe_execute() only fails on an inter-core handshake
        // timeout) -- this chunk stays marked "received" regardless, since
        // this protocol has no per-chunk NACK. UPDATE_END's whole-slot
        // read-back CRC (item 10.7) is the backstop that would catch a
        // write that "reported success and did not land"; latch this so a
        // human reading UPDATE_STATUS sees SOMETHING was wrong even before
        // that final check runs.
        s_sticky_error |= UPDATE_STATUS_ERR_INTERNAL;
    }
}

// --- UPDATE_END ------------------------------------------------------------

static void update_task_process_end(const uint8_t *payload, uint8_t length)
{
    if (!s_transfer_active) {
        return; // discard, same reasoning as UPDATE_DATA above
    }
    if (length < 1u + 4u) {
        update_task_send_status_now(UPDATE_TASK_STATE_RECEIVING, UPDATE_STATUS_ERR_INTERNAL);
        return;
    }

    uint32_t end_crc = get_u32_le(&payload[1]);

    if (!update_received_ranges_is_complete(&s_ranges)) {
        // Keep waiting -- the periodic gap report already names what is
        // missing; this reply just confirms "not done yet" rather than
        // silently ignoring a premature END.
        update_task_send_status_now(UPDATE_TASK_STATE_RECEIVING, 0);
        return;
    }

    update_task_send_status_now(UPDATE_TASK_STATE_VERIFYING, 0);

    // Item 10.7: read back from flash (XIP-mapped, same technique
    // bootloader/main.c's step 5 CRC check uses) and CRC what was ACTUALLY
    // WRITTEN, not what the receive-side bitmap thinks arrived --
    // docs/BOOTLOADER.md section 5: "the only check that catches a write
    // that reported success and did not land."
    const uint8_t *slot_data = (const uint8_t *)(XIP_BASE + s_slot_flash_offset);
    uint32_t actual_crc = bootloader_crc32(slot_data, s_header.length);

    if (end_crc != s_header.crc32) {
        // The END frame's repeated CRC should equal the BEGIN header's own
        // crc32 field (UPDATE_PROTOCOL.md section 4: "image CRC32 repeated").
        // A mismatch between the two is the ESP's own bug, not evidence
        // about what actually landed in flash -- log it (no log_task call
        // site wired up in this file this pass) and continue trusting the
        // header's crc32 as ground truth for the read-back comparison below,
        // since that is the value update_receiver_handle_begin() already
        // validated the image length against.
    }

    if (actual_crc != s_header.crc32) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_CRC_MISMATCH);
        update_task_revert_target_slot();
        return;
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata_or_default(&meta);
    meta.slots[s_target_slot].state = BOOTLOADER_SLOT_PENDING_VERIFY;
    meta.slots[s_target_slot].length = s_header.length;
    meta.slots[s_target_slot].crc32 = s_header.crc32;
    memcpy(meta.slots[s_target_slot].version, s_header.version, sizeof(meta.slots[s_target_slot].version));
    memset(meta.slots[s_target_slot].build_commit, 0, sizeof(meta.slots[s_target_slot].build_commit));
    meta.slots[s_target_slot].build_epoch = 0; // unknown -- image_header.h carries no build timestamp field

    // The new image only gets picked up on the NEXT boot if the bootloader's
    // own bootloader_decide_boot() actually considers this slot -- which it
    // only does if active_slot names it (metadata.c: "if
    // slot_is_bootable(meta->slots[active].state) ... boot active"). Leaving
    // active_slot pointed at the OLD slot here would silently make this
    // whole write pointless: the next boot (whenever a future reboot trigger
    // fires -- see this function's closing comment) would boot the old
    // image again, never even attempting the one just verified. So this is
    // deliberately part of "a verified write", not a separate decision:
    // flip active_slot to the target and reset boot_attempts to 0 (a fresh
    // slot gets a fresh attempt budget; bootloader_decide_boot() increments
    // it to 1 on the boot that actually tries it).
    meta.active_slot = s_target_slot;
    meta.boot_attempts = 0;

    if (!update_task_persist_metadata(&meta, latest)) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_INTERNAL);
        return;
    }

    update_task_send_status_now(UPDATE_TASK_STATE_COMPLETE, 0);
    s_transfer_active = false;

    // Deliberately NOT rebooting here. UPDATE_PROTOCOL.md section 4 flow
    // step 6 says "Pico marks the slot pending and reboots" -- the metadata
    // write above is that first half. Restarting a live safety processor
    // mid-operation (guards were still running the whole time this transfer
    // was in progress -- nothing in this task touches the relay or the
    // guard path) is exactly the kind of consequential action this task's
    // brief asks to be careful about rather than rush. Left as an explicit
    // follow-on: the likely mechanism is a software reset via AIRCR
    // (Cortex-M SCB->AIRCR SYSRESETREQ) or pico-sdk's watchdog_reboot(),
    // triggered either by a future ESP-initiated "reboot now" frame or by
    // this task itself once it has confirmed there is nothing else
    // in-flight -- not built this pass.
}

// --- UPDATE_ABORT ------------------------------------------------------------

static void update_task_process_abort(void)
{
    update_task_revert_target_slot(); // no-op if no transfer is active
    update_task_send_status_now(UPDATE_TASK_STATE_ABORTED, 0);
}

// --- Periodic gap-report status while a transfer is active (item 10.8c) ---

static void update_task_abort_internal(uint8_t err)
{
    update_task_revert_target_slot();
    update_task_send_status_now(UPDATE_TASK_STATE_FAILED, err);
}

// Called from update_task_fn()'s own loop on UPDATE_STATUS_TX_PERIOD_MS,
// only while s_transfer_active. Advances the gap-report cursor per
// received_ranges.h's own documented scheme (find_gaps() is pure and does
// not track the cursor itself -- this is the caller doing that): one call
// reports up to UPDATE_STATUS_MAX_GAPS missing chunk indices starting at
// s_gap_cursor, wrapping to 0 once a full pass reaches total_chunks. A full
// pass that still found at least one gap counts as one retransmission round
// (update_receiver.h's own definition: "one 'round' = one full gap-report
// cycle... that still finds at least one gap when it completes") --
// s_pass_had_gap tracks whether THIS pass (since the cursor last wrapped)
// has found anything missing yet, so a pass that starts finding gaps only
// partway through still counts once it wraps, and a pass that finds nothing
// at all (the transfer is actually complete, or about to be) does not.
static void update_task_periodic_status(void)
{
    if (!s_transfer_active) {
        return;
    }
    if (update_received_ranges_is_complete(&s_ranges)) {
        return; // UPDATE_END will finish this transfer; nothing to report
    }

    uint32_t gaps[UPDATE_STATUS_MAX_GAPS];
    size_t gap_count =
        update_received_ranges_find_gaps(&s_ranges, s_gap_cursor, gaps, UPDATE_STATUS_MAX_GAPS);

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
        update_task_abort_internal(UPDATE_STATUS_ERR_RETRANSMIT_CAP);
        return;
    }

    update_task_pack_and_send(UPDATE_TASK_STATE_RECEIVING, 0, gaps, gap_count);
}

// --- Confirmation gate (item 10.8) ------------------------------------------

static void update_task_startup_confirm_check(void)
{
    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata(&meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT) {
        return; // no metadata at all -- e.g. the plain (non-slot-linked) SaftyFW dev target, or a never-updated board. Nothing to confirm.
    }
    if (meta.active_slot >= BOOTLOADER_SLOT_COUNT) {
        return; // malformed record -- should not happen (metadata_unpack() would already have rejected obviously bad content), defensive only
    }
    if (meta.slots[meta.active_slot].state == BOOTLOADER_SLOT_PENDING_VERIFY) {
        s_confirm_pending = true;
        s_own_slot = meta.active_slot;
    }
}

// Gathers real evidence (src/update/confirm.h's update_confirm_checklist_t)
// and, once update_confirm_missing() returns 0, marks this slot VALID
// exactly once. NOTE, stated here and in TODO.md: config_crc_ok is
// permanently false in this build (no config_store, Phase 9 -- confirm.h's
// own doc comment: "a caller with nothing to check should pass false, never
// true") so update_confirm_missing() can never actually reach 0 here. This
// is an honest consequence of building the confirmation gate against a
// config_store that does not exist yet, not a bug -- the gate is fully
// wired and will start working the moment Phase 9 lands a real config CRC
// check, with no further change needed in this file.
static void update_task_confirm_tick(void)
{
    if (!s_confirm_pending) {
        return;
    }

    update_confirm_checklist_t c;
    c.config_crc_ok = false; // see this function's header comment -- never fake this

    thermo_snapshot_t th;
    bool th_present = thermo_task_get_snapshot(&th);
    c.thermocouple_plausible = th_present && th.valid;

    current_snapshot_t cur;
    current_task_get_snapshot(&cur);
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    c.adc_sampling_ok =
        (cur.timestamp_ms != 0u) && ((now_ms - cur.timestamp_ms) < UPDATE_TASK_ADC_FRESH_MS);

    c.all_tasks_checked_in = watchdog_task_all_checked_in_since_boot();
    c.telemetry_sent_ok = link_task_get_status_tx_ok_count() > 0u;

    if (update_confirm_missing(&c) != 0u) {
        return; // still missing something -- see this function's header comment on config_crc_ok
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata(&meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT || s_own_slot >= BOOTLOADER_SLOT_COUNT) {
        return; // nothing to mutate -- retry on a later tick
    }
    meta.slots[s_own_slot].state = BOOTLOADER_SLOT_VALID;
    if (update_task_persist_metadata(&meta, latest)) {
        s_confirm_pending = false; // confirmed -- nothing further to do, ever, this boot
    }
    // On persist failure, s_confirm_pending stays true and this retries on
    // the next UPDATE_CONFIRM_TICK_PERIOD_MS tick.
}

// --- Explicit rollback (SAFETY_CMD_ROLLBACK, 0x17) --------------------------
//
// tools/PcTools/TODO.md's `ota_rollback(processor)` line, Pico half (the ESP
// half already exists: ota_http.c's POST /api/ota/esp/rollback). Called
// SYNCHRONOUSLY from link_task_handle_rollback() (src/tasks/link_task.c) --
// not queued to this task's own s_rx_queue like UPDATE_BEGIN/_DATA/_END/
// _ABORT are -- following config_store_write()'s own established precedent
// (link_task_handle_set_config() already calls it directly, doing a real
// flash_safe_execute() write on link_task's own priority/stack) rather than
// UPDATE_*'s "flash I/O does not belong on link_task's priority/stack"
// reasoning, which was written for erasing/programming up to 832K over many
// blocks -- a single metadata record write is the same small, bounded cost
// config_store_write() already accepts inline.
//
// Returns false (filling `*out_reason`) on any refusal or a flash failure --
// the caller only logs it, exactly like config_store_write()'s own
// accept/refuse logging, since this frame never ACKs on the wire (same as
// CLEAR_TRIP/SET_CONFIG). On success this function DOES NOT RETURN:
// watchdog_reboot() resets the RP2040 immediately, which is the entire
// point of the command.
bool update_task_request_rollback(const char **out_reason)
{
    // Same ARMED-equivalent gate config_store_write() uses (relay_owner_
    // get_state() == RELAY_OWNER_STATE_ARMED there; this file legitimately
    // reaches relay state only through safety_core_get_output_status(), the
    // same legal channel update_task_gather_preconditions() already uses
    // for UPDATE_BEGIN's own relay_open precondition -- see this file's
    // header comment on why update_task.c may call safety_core but not
    // relay_owner.h directly). A rollback reboots into different code, which
    // is exactly as disruptive as an update or a config write while heating
    // is armed.
    bool relay_energized = false;
    safety_core_get_output_status(&relay_energized, NULL);
    if (relay_energized) {
        if (out_reason) {
            *out_reason = "refused: relay is ARMED, rollback is refused while ARMED "
                          "(same gate as config writes)";
        }
        return false;
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata(&meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT) {
        if (out_reason) {
            *out_reason = "refused: no bootloader metadata to roll back from";
        }
        return false;
    }
    if (meta.active_slot >= BOOTLOADER_SLOT_COUNT) {
        if (out_reason) {
            *out_reason = "refused: malformed metadata (active_slot out of range)";
        }
        return false;
    }

    // meta.active_slot -- not s_own_slot above -- is the authoritative
    // "which slot actually booted" fact: bootloader_decide_boot()
    // (bootloader/metadata.c) persists it every time it chooses or falls
    // back to a slot, so it stays correct for the whole life of a boot,
    // including the common steady-state case (an already-confirmed VALID
    // slot) where s_own_slot is never updated past its BOOTLOADER_SLOT_A
    // start-of-day default -- update_task_startup_confirm_check() only sets
    // s_own_slot when THIS boot's active slot is PENDING_VERIFY. Using
    // s_own_slot here would silently mark the wrong slot BAD on every boot
    // that never needed the confirmation gate at all.
    bootloader_rollback_decision_t decision = bootloader_decide_rollback(&meta, meta.active_slot);
    if (!decision.allowed) {
        // The one property that matters most for this whole feature: refuse
        // rather than strand the board with zero bootable slots. See
        // bootloader_decide_rollback()'s own doc comment (metadata.h) for
        // exactly what "the other slot" means and why this check runs
        // before anything is marked BAD.
        if (out_reason) {
            *out_reason = "refused: the other bootloader slot is not currently valid "
                          "to fall back to";
        }
        return false;
    }

    if (!update_task_persist_metadata(&decision.updated_meta, latest)) {
        if (out_reason) {
            *out_reason = "flash write failed";
        }
        return false;
    }

    if (out_reason) {
        *out_reason = "ok";
    }

    // No further code in this function runs after this call -- the caller
    // must log "accepted" (or otherwise act on `out_reason == "ok"`) BEFORE
    // calling this function's caller chain concludes, since watchdog_reboot()
    // resets the RP2040 immediately rather than returning.
    watchdog_reboot(0, 0, 0);
    for (;;) {
        // Defensive only: watchdog_reboot() does not return on real
        // hardware. Never reached, but a function declared to return bool
        // must not fall off its own end.
    }
}

// --- Queue-facing handlers (called from link_task's context) --------------

static void update_task_enqueue(const uint8_t *payload, uint8_t length)
{
    if (!s_rx_queue || length == 0u || length > KILNLINK_FRAME_MAX_PAYLOAD) {
        return;
    }
    update_task_msg_t msg;
    msg.length = length;
    memcpy(msg.payload, payload, length);
    // Zero-timeout: never blocks link_task's RX loop. A full queue means
    // update_task has fallen far behind (e.g. mid-erase, which halts both
    // cores anyway) -- the frame is dropped, matching this codebase's
    // "never block, never allocate after init" discipline. No counter
    // exposed for this yet (see update_task.h's own comment on that).
    (void)xQueueSend(s_rx_queue, &msg, 0);
}

void update_task_handle_begin(const uint8_t *payload, uint8_t length)
{
    update_task_enqueue(payload, length);
}

void update_task_handle_data(const uint8_t *payload, uint8_t length)
{
    update_task_enqueue(payload, length);
}

void update_task_handle_end(const uint8_t *payload, uint8_t length)
{
    update_task_enqueue(payload, length);
}

void update_task_handle_abort(const uint8_t *payload, uint8_t length)
{
    update_task_enqueue(payload, length);
}

// --- Task --------------------------------------------------------------------

static void update_task_dispatch(const update_task_msg_t *msg)
{
    if (msg->length == 0u) {
        return;
    }
    uint8_t cmd = msg->payload[0];
    switch (cmd) {
    case LINK_FRAME_UPDATE_BEGIN_CMD:
        update_task_process_begin(msg->payload, msg->length);
        break;
    case LINK_FRAME_UPDATE_DATA_CMD:
        update_task_process_data(msg->payload, msg->length);
        break;
    case LINK_FRAME_UPDATE_END_CMD:
        update_task_process_end(msg->payload, msg->length);
        break;
    case LINK_FRAME_UPDATE_ABORT_CMD:
        update_task_process_abort();
        break;
    default:
        break; // cannot happen -- link_task.c only enqueues these four commands
    }
}

static void update_task_fn(void *arg)
{
    (void)arg;

    update_task_startup_confirm_check();

    TickType_t last_status_tx = xTaskGetTickCount();
    TickType_t last_confirm_tick = xTaskGetTickCount();

    for (;;) {
        update_task_msg_t msg;
        while (xQueueReceive(s_rx_queue, &msg, 0) == pdTRUE) {
            update_task_dispatch(&msg);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_tx) >= pdMS_TO_TICKS(UPDATE_STATUS_TX_PERIOD_MS)) {
            update_task_periodic_status();
            last_status_tx = now;
        }
        if ((now - last_confirm_tick) >= pdMS_TO_TICKS(UPDATE_CONFIRM_TICK_PERIOD_MS)) {
            update_task_confirm_tick();
            last_confirm_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(UPDATE_TASK_POLL_MS));

        watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
    }
}

bool update_task_start(void)
{
    s_transfer_active = false;
    s_confirm_pending = false;
    s_own_slot = BOOTLOADER_SLOT_A;
    s_sticky_error = 0;

    s_rx_queue = xQueueCreate(UPDATE_TASK_QUEUE_DEPTH, sizeof(update_task_msg_t));
    if (!s_rx_queue) {
        return false;
    }

    BaseType_t ok = xTaskCreate(update_task_fn, "update_task", UPDATE_TASK_STACK_WORDS, NULL,
                                 SAFTYFW_PRIO_UPDATE_TASK, &s_task_handle);
    if (ok != pdPASS) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SAFTYFW_CORE_LINK_PATH);
    return true;
}
