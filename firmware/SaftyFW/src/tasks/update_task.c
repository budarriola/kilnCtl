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

#include "pico/time.h"

// hardware/watchdog.h dropped earlier: hal_wdt.h covers this file's sole
// watchdog_reboot(0, 0, 0) call, see update_task_request_rollback() below.
// hardware/flash.h, hardware/regs/addressmap.h (XIP_BASE), and pico/flash.h
// dropped THIS pass: every flash_range_erase()/flash_range_program()/
// XIP_BASE/flash_safe_execute() call below now goes through hal_flash.h
// instead -- the rebase hal_flash.h's own header comment had deferred
// ("its rebase is not scheduled by the current plan pass") is done. See
// update_task_ensure_flash_region() below for the region binding and
// hal_flash_map()'s own doc comment (hal_flash.h) for why UPDATE_END's
// whole-slot CRC read-back needed a new HAL primitive rather than
// hal_flash_read() alone.
#include "hal_flash.h"
#include "hal_wdt.h"

#include "task_priorities.h"
#include "update_task_erase_plan.h"
#include "update_task_slot_linkage.h"
#include "watchdog_task.h"

#include "current_task.h"
#include "link_frame.h"
#include "link_task.h"
#include "log_task.h" // persist/logging audit (2026-09-06): update_task_persist_metadata()
                       // failures previously reached only the wire status frame
                       // (UPDATE_TASK_STATE_FAILED) with no on-device log line naming
                       // the module -- see this file's own comment (formerly here,
                       // now resolved) on update_task_process_begin()'s
                       // decision.requested_slot_mismatch line.
#include "safety_core.h"
#include "snapshots.h"
#include "thermo_task.h"

#include "config_store.h" // config_store_get_config_version()/config_store_confirm_crc_ok()
#include "crc32.h"       // bootloader/ -- see CMakeLists.txt include path
#include "flash_layout.h" // bootloader/
#include "metadata.h"     // bootloader/

#include "confirm.h"         // src/update/
#include "image_header.h"    // src/update/
#include "received_ranges.h" // src/update/
#include "update_receiver.h" // src/update/

#include "update_task_metadata_write.h" // audit L4: erased check + read-back
#include "update_task_flash_guard.h" // 2026-09-21 -- see this file's own header comment
                                      // on update_task_running_image_flash_range() below

#include "kilnlink/kilnlink_frame.h" // KILNLINK_FRAME_MAX_PAYLOAD
#include "kilnlink/kilnlink_reboot_result.h" // kilnlink_reboot_result_reason_t, for update_task_reboot_allowed()'s out_reason_code
#include "kilnlink/kilnlink_rollback_result.h" // kilnlink_rollback_result_reason_t, for update_task_request_rollback()'s out_reason_code

#include "update_task_reboot_policy.h" // update_task_reboot_policy_decide() -- the pure,
                                        // host-testable half of update_task_reboot_allowed()

// 2026-09-10 (owner decision "raise both stacks"): check_saftyfw_task_stack_
// budgets.py's regsp-margin check FAILED at *3 -- measured 2536 B lower
// bound needs declared >= 5072 B for that check to clear, and *3 (3072 B)
// was only 83% of that. Bumped to *6 (6144 B), the same headroom
// current_task/safety_core already carry, same "RAM is cheap" reasoning as
// link_task.c's own LINK_TASK_STACK_WORDS comment. configTOTAL_HEAP_SIZE
// (FreeRTOSConfig.h) raised in the same commit to keep non-stack heap
// headroom sane after this and link_task's stacks both grew.
#define UPDATE_TASK_STACK_WORDS (configMINIMAL_STACK_SIZE * 6) // page_buf below is 512 bytes
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

// The unit update_task_erase_slot() erases at a time: one HAL_FLASH_ERASE_SIZE
// sector, the SMALLEST erase this part supports, which is what bounds a single
// uninterruptible stall.
//
// 2026-09-18 (docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md): this
// was 64 KB -- pico-sdk's FLASH_BLOCK_SIZE, reproduced locally -- and that is
// what hardware-watchdog-reset the safety processor partway through every
// ESP-driven OTA. Nothing can feed the watchdog while hal_flash_safe_execute()
// runs (it holds BOTH cores with interrupts disabled), so the erase unit IS
// the stall, and a 64 KB block erase on this part's flash is specified well
// past the 1000 ms SAFTYFW_WATCHDOG_TIMEOUT_MS. A 4 KB sector erase is the
// smallest stall the part can be asked for.
//
// 4096 is not a free choice: HAL_FLASH_ERASE_SIZE (hal_flash.h) is the
// granularity hal_flash_erase() already range-checks every offset and length
// against, and hal_flash_pico.c pins it to pico-sdk's own FLASH_SECTOR_SIZE
// with a compile-time cross-check (the hal_flash_erase_size_matches_pico_sdk
// typedef), so nothing smaller is expressible through this HAL at all.
// BOOTLOADER_SLOT_FLASH_SIZE (0xD0000) is exactly 208 * this with no
// remainder (flash_layout.h) -- the _Static_assert below is what actually
// enforces that, at whatever value this macro holds.
#define UPDATE_TASK_ERASE_CHUNK_SIZE (4u * 1024u)

// 2026-09-06 review fix: the old code got this cross-check for free --
// hardware/flash.h's FLASH_BLOCK_SIZE was a real pico-sdk constant, and the
// "13 * FLASH_BLOCK_SIZE with no remainder" fact update_task_erase_slot()'s
// own header comment states was true of THAT constant by construction. Now
// that this file defines its own chunk size, the same fact must be checked,
// not just asserted in a comment -- a future edit to either
// BOOTLOADER_SLOT_FLASH_SIZE (flash_layout.h) or this macro that broke exact
// divisibility would otherwise silently leave update_task_erase_slot()'s
// last chunk smaller than UPDATE_TASK_ERASE_CHUNK_SIZE but still passed
// through hal_flash_erase() unaligned-length-checked (HAL_FLASH_ERASE_SIZE
// alignment, not this chunk size) -- functionally survivable, but a quiet
// change to the erase/watchdog-feeding cadence this code was deliberately
// tuned around.
_Static_assert(BOOTLOADER_SLOT_FLASH_SIZE % UPDATE_TASK_ERASE_CHUNK_SIZE == 0,
               "UPDATE_TASK_ERASE_CHUNK_SIZE must evenly divide BOOTLOADER_SLOT_FLASH_SIZE "
               "(flash_layout.h) -- update_task_erase_slot()'s own header comment relies on "
               "this exactly like the old FLASH_BLOCK_SIZE-based code did");

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
    // Owner decision 2026-09-20 (Pico-side slot-linkage validation, see
    // update_task_slot_linkage_plausible()'s header comment below): a
    // DISTINCT wire state for "the image CRCed correctly but its vector
    // table is not plausible for the slot it was just written into" --
    // e.g. a slot-A-linked image landed in slot B, or vice versa. This is
    // a NEW legal value of the existing 1-byte `state` field, not a wire
    // layout change, so it needs no protocol-version bump: the `last_error`
    // bitmask field (below) has no spare bit left (all 8 already assigned),
    // which is why this distinct signal lives in `state` instead. The
    // accompanying `last_error` byte still reports
    // UPDATE_STATUS_ERR_CRC_MISMATCH -- not because the CRC actually
    // mismatched, but because it is the closest existing bit: both cases
    // are "the just-verified image failed a validity check performed on
    // its landed bytes, right before the metadata flip that would make it
    // active" (see update_task_process_end()'s call site). A caller that
    // only reads `last_error` sees the same bit a genuine CRC failure
    // would set; a caller that also reads `state` can tell the two apart
    // by this value. (See the coordinator's task brief: a codec/name table
    // for this value in tools/PcTools is a separate agent's follow-up.)
    UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE = 8,

    // 2026-09-21 (triage finding, running-image flash-overlap guard -- see
    // update_task_flash_guard.h's header comment): a DISTINCT wire state for
    // "this board's own running image occupies flash the requested operation
    // would have to erase or program" -- observed on a bench Pico running a
    // flat, bootloader-less image loaded at XIP_BASE, whose extent overlapped
    // BOOTLOADER_METADATA_FLASH_OFFSET and part of slot A. Same reasoning as
    // UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE immediately above: a NEW legal
    // value of the existing 1-byte `state` field, not a wire layout change,
    // so no protocol-version bump is needed. The accompanying `last_error`
    // byte still reports UPDATE_STATUS_ERR_INTERNAL -- the closest existing
    // bit (all 8 are already assigned) -- for a caller that only reads
    // `last_error`; a caller that also reads `state` can tell this apart from
    // an ordinary internal erase/program failure. NOTE for the ESP side
    // (KilnFW, not touched by this change): safety_link.h's mirrored
    // SAFETY_LINK_UPDATE_STATE_* enum and ota_pico_relay.c's handling of
    // SAFETY_LINK_UPDATE_STATE_REJECTED_SLOT_LINKAGE (the same-shaped prior
    // addition) need a matching SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_
    // IMAGE_OVERLAP = 9 and a render/handling path, or this state number
    // reaches the ESP unrecognised.
    UPDATE_TASK_STATE_REFUSED_RUNNING_IMAGE_OVERLAP = 9,
} update_task_wire_state_t;

// last_error bits. The first three mirror update_receiver.h's
// update_precondition_flag_t bit-for-bit (see update_task_precond_to_wire()
// below) so a caller that already knows that enum recognises them; the rest
// are this frame's own additions for outcomes update_precondition_flag_t has
// no bit for. All 8 bits of this field are now assigned -- see
// UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE above for why a new failure mode
// added 2026-09-20 reuses CRC_MISMATCH here rather than getting its own bit,
// and UPDATE_TASK_STATE_REFUSED_RUNNING_IMAGE_OVERLAP (added 2026-09-21) for
// why that one reuses INTERNAL the same way.
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
// volatile: read cross-task by update_task_transfer_active() (safety_core.c)
// -- see update_task.h's doc comment on that getter for why a torn/stale
// read here is safe-direction-only.
static volatile bool s_transfer_active = false;
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

// --- Active-slot cache for update_task_get_active_slot() (docs/
// PICO_AUTO_UPDATE.md:64) -- populated once, at startup, by
// update_task_startup_confirm_check() below; NOT the same thing as
// s_own_slot above (see update_task_get_active_slot()'s own doc comment in
// update_task.h for why that cache cannot be reused here).
static bool s_active_slot_known = false;
static uint8_t s_active_slot = BOOTLOADER_SLOT_A;

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

// --- hal_flash region binding ------------------------------------------------
//
// Unlike config_store_flash.c (rebased the same pass), which binds one
// region to its own single 4K sector, this file touches three disjoint
// regions of flash (the metadata sector, slot A, slot B) at offsets that
// are only known at a given call site (s_slot_flash_offset varies per
// transfer). Rather than one hal_flash_region_t per named region, this
// binds ONE region spanning the WHOLE chip (base 0, size
// BOOTLOADER_FLASH_TOTAL_SIZE) and keeps every offset below exactly as it
// was before the rebase -- an absolute, flash-start-relative offset, the
// same convention flash_layout.h's own macros and the old bare XIP_BASE +
// offset arithmetic already used. This is the minimal-diff choice: no
// offset math anywhere in this file needed to change to become
// region-relative.
static hal_flash_region_t s_flash_region;
static bool s_flash_region_ready = false;

// --- Running-image flash-overlap guard (2026-09-21 triage finding) ---------
//
// __flash_binary_start/__flash_binary_end are linker symbols this file's own
// linker script (bootloader/app_slot.ld.in's `.flash_begin`/`.flash_end`
// sections) places at the very first and very last byte of THIS executable's
// own flash image -- and, per that file's own header comment, this shape is
// copied unchanged from the stock pico-sdk linker script
// (memmap_default.ld), so the same two symbols exist and mean the same thing
// in a flat, bootloader-less build too. Because this is XIP-resident code,
// "where the linker placed it" and "where it is actually running from right
// now" are the same address by construction -- there is no copy/relocate
// step to make them drift apart. Reading them at runtime is therefore this
// firmware's own ground truth for its own extent, not a guess about what
// might be running.
//
// This is exactly the fact the triage finding turns on: a normal slot-linked
// build's extent starts at BOOTLOADER_SLOT_A/B_FLASH_OFFSET and cannot reach
// back to the metadata sector or the OTHER slot, so this guard is inert for
// the ordinary two-slot path. A flat/bootloader-less build's extent starts
// at flash offset 0 and can overlap the metadata sector and part of slot A
// -- exactly what corrupted the bench board's own running code when
// update_task_persist_metadata() programmed the metadata sector out from
// under it.
extern uint8_t __flash_binary_start;
extern uint8_t __flash_binary_end;

// XIP_BASE as a local literal, same reasoning and same value as
// UPDATE_TASK_SLOT_LINKAGE_XIP_BASE below (architectural RP2040 constant,
// restated here rather than pulled from pico-sdk's hardware/regs/
// addressmap.h so this file's existing HAL-include-boundary allowlist does
// not need a new entry).
#define UPDATE_TASK_FLASH_GUARD_XIP_BASE 0x10000000u

// Fills *out_start_offset/*out_end_offset with this running image's own
// flash-relative extent (flash_layout.h's own offset convention -- add
// XIP_BASE for an execute-in-place pointer), i.e. exactly the two arguments
// update_task_flash_guard_overlaps() needs as its "running image" side.
static void update_task_running_image_flash_range(uint32_t *out_start_offset, uint32_t *out_end_offset)
{
    *out_start_offset = (uint32_t)&__flash_binary_start - UPDATE_TASK_FLASH_GUARD_XIP_BASE;
    *out_end_offset = (uint32_t)&__flash_binary_end - UPDATE_TASK_FLASH_GUARD_XIP_BASE;
}

// Single call site every erase/program path below funnels through: true iff
// the given flash-relative region would touch any byte of this running
// image's own extent, in which case the caller MUST refuse outright --
// never erase, never program, regardless of which region or which path
// reached this point. Also refuses (fails closed, returns true) if this
// running image's own extent cannot be trusted (malformed symbols) --
// update_task_flash_guard_running_image_extent_valid()'s own doc comment
// explains why an unusable extent must never be read as "no overlap".
static bool update_task_region_overlaps_running_image(uint32_t region_offset, uint32_t region_size)
{
    uint32_t image_start = 0;
    uint32_t image_end = 0;
    update_task_running_image_flash_range(&image_start, &image_end);

    if (!update_task_flash_guard_running_image_extent_valid(image_start, image_end)) {
        return true; // fail closed -- see this function's own header comment
    }

    return update_task_flash_guard_overlaps(region_offset, region_size, image_start, image_end);
}

static bool update_task_ensure_flash_region(void)
{
    if (s_flash_region_ready) {
        return true;
    }
    if (hal_flash_region_init(&s_flash_region, 0u, BOOTLOADER_FLASH_TOTAL_SIZE) != HAL_OK) {
        return false;
    }
    s_flash_region_ready = true;
    return true;
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

// NOT a module-scope staging buffer any more (2026-09-06 review fix): this
// function is called from TWO task contexts, not one -- update_task_fn()'s
// own dispatch loop (UPDATE_BEGIN/_END/_ABORT) AND
// update_task_request_rollback(), which link_task_handle_rollback() calls
// SYNCHRONOUSLY on link_task's own context (see this file's own comment on
// update_task_request_rollback() above, and link_task.c's call site). A
// single static uint8_t[BOOTLOADER_METADATA_FLASH_SIZE] shared by both would
// let one task's in-progress hal_flash_read() be torn by the other task
// preempting mid-copy, which bootloader_metadata_find_latest() would then
// read as "no slot has a valid CRC" -- and update_task_read_latest_metadata_
// or_default() turns THAT into a freshly-bootstrapped default record
// (active_slot = A) that a caller may go on to persist, silently repointing
// the boot target. This is new exposure the pre-rebase code never had: it
// read straight from the XIP-mapped alias into the CALLER's own local
// bootloader_metadata_t-sized stack buffer (bootloader_metadata_find_latest()
// takes a `const uint8_t sector[...]` view, not an owning buffer), with no
// staging step at all for two callers to race over. hal_flash_map() (added
// alongside this rebase for exactly this "read a live flash view without
// copying" shape) restores that: no buffer, no cross-task race, and it costs
// less RAM than the deleted static ever did.
static size_t update_task_read_latest_metadata(bootloader_metadata_t *out_meta)
{
    const void *sector_ptr = NULL;
    if (!update_task_ensure_flash_region() ||
        hal_flash_map(&s_flash_region, BOOTLOADER_METADATA_FLASH_OFFSET,
                       BOOTLOADER_METADATA_FLASH_SIZE, &sector_ptr) != HAL_OK) {
        // Matches the old bare-XIP-pointer path's own failure mode: XIP reads
        // never actually failed before (the region was always addressable
        // memory), so this branch is new, but bootloader_metadata_find_latest()
        // already treats "not a single valid record" as an ordinary, expected
        // outcome (BOOTLOADER_METADATA_NO_SLOT) -- an unmappable region gets
        // exactly that same answer rather than a distinct error path.
        return BOOTLOADER_METADATA_NO_SLOT;
    }
    return bootloader_metadata_find_latest((const uint8_t *)sector_ptr, out_meta);
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

_Static_assert(BOOTLOADER_METADATA_RECORD_LEN <= UPDATE_METADATA_MAX_RECORD,
               "metadata record must fit update_task_metadata_write_verified()'s verify buffer");

typedef struct {
    size_t next_write_slot;
    bool needs_erase;
    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    hal_status_t result; // 2026-09-06 review fix: the inner erase/program status
                          // must survive the callback -- see this struct's
                          // only reader, update_task_persist_metadata(), for
                          // why hal_flash_safe_execute()'s own HAL_OK is not
                          // enough on its own (it only reports whether the
                          // callback RAN, not whether the op it ran
                          // succeeded -- pico's flash_range_erase()/_program()
                          // are void, but the hal_flash_* wrappers around them
                          // do return a real status now, and discarding it
                          // would let a failed erase/program still report
                          // success up the call chain).
} update_metadata_write_args_t;

static bool update_meta_io_erase(void *ctx)
{
    (void)ctx;
    return hal_flash_erase(&s_flash_region, BOOTLOADER_METADATA_FLASH_OFFSET,
                           BOOTLOADER_METADATA_FLASH_SIZE) == HAL_OK;
}

static bool update_meta_io_read(void *ctx, uint32_t offset, uint8_t *buf, size_t len)
{
    (void)ctx;
    return hal_flash_read(&s_flash_region, BOOTLOADER_METADATA_FLASH_OFFSET + offset, buf,
                          len) == HAL_OK;
}

static bool update_meta_io_program(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    return hal_flash_program(&s_flash_region, BOOTLOADER_METADATA_FLASH_OFFSET + offset, data,
                             len) == HAL_OK;
}

// Audit L4 (UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09): erase, erased-slot
// check, program and byte-for-byte read-back all live in
// update_task_metadata_write_verified() (host-tested); a failed step maps to
// HAL_IO here so the caller never reports COMPLETE.
static void update_metadata_write_cb(void *param)
{
    update_metadata_write_args_t *a = (update_metadata_write_args_t *)param;
    const update_metadata_write_io_t io = {
        .erase = update_meta_io_erase,
        .read = update_meta_io_read,
        .program = update_meta_io_program,
        .ctx = NULL,
    };
    update_metadata_write_result_t r = update_task_metadata_write_verified(
        &io, a->needs_erase, (uint32_t)a->next_write_slot * BOOTLOADER_METADATA_RECORD_LEN,
        a->record, BOOTLOADER_METADATA_RECORD_LEN);
    a->result = (r == UPDATE_METADATA_WRITE_OK) ? HAL_OK : HAL_IO;
}

// Mirrors bootloader/main.c's persist_metadata(meta, latest_slot) exactly in
// shape (same seq-increment-once, next-write-slot, erase-if-wrapped, pack
// steps) but flash_safe_execute()-wrapped instead of a bare interrupt
// critical section, with watchdog checkins immediately before and after --
// this is a single ~256-byte-page write, nowhere near the erase's
// multi-hundred-millisecond cost, but the discipline is the same either way.
static bool update_task_persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index)
{
    if (!update_task_ensure_flash_region()) {
        return false;
    }

    // Assert-like guard (2026-09-21 triage finding): never erase or program
    // the metadata sector if doing so would touch this board's own running
    // image. See update_task_region_overlaps_running_image()'s header
    // comment -- this is the exact operation that corrupted the bench board
    // when it was running a flat, bootloader-less image whose extent
    // overlapped BOOTLOADER_METADATA_FLASH_OFFSET.
    if (update_task_region_overlaps_running_image(BOOTLOADER_METADATA_FLASH_OFFSET,
                                                    BOOTLOADER_METADATA_FLASH_SIZE)) {
        log_task_log(LOG_LEVEL_ERROR, "update",
                     "persist_metadata: refused -- metadata sector overlaps running image");
        return false;
    }

    meta->seq = meta->seq + 1u;

    update_metadata_write_args_t args;
    args.next_write_slot = bootloader_metadata_next_write_slot(latest_slot_index);
    args.needs_erase = bootloader_metadata_next_write_needs_erase(latest_slot_index);
    bootloader_metadata_pack(meta, args.record);

    args.result = HAL_NOT_READY; // overwritten by the callback if it ever runs
    watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
    hal_status_t status = hal_flash_safe_execute(update_metadata_write_cb, &args, 1000u);
    watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
    // Both must succeed: hal_flash_safe_execute() reports whether the
    // callback ran at all (lockout handshake), args.result reports whether
    // the erase/program it ran actually landed -- see args.result's own doc
    // comment on update_metadata_write_args_t for why neither check alone is
    // sufficient.
    return status == HAL_OK && args.result == HAL_OK;
}

// --- Flash erase (UPDATE_BEGIN acceptance) ---------------------------------

typedef struct {
    uint32_t offset;
    size_t count;
    hal_status_t result; // 2026-09-06 review fix -- see update_metadata_write_args_t's
                          // own result field comment for why this must be
                          // checked, not discarded.
} update_erase_args_t;

static void update_erase_cb(void *param)
{
    update_erase_args_t *a = (update_erase_args_t *)param;
    a->result = hal_flash_erase(&s_flash_region, a->offset, a->count);
}

// Erases the whole BOOTLOADER_SLOT_FLASH_SIZE (832K) target slot one
// UPDATE_TASK_ERASE_CHUNK_SIZE (4K, one flash sector) at a time: one
// hal_flash_safe_execute() call per sector, a watchdog checkin before AND
// after each, and -- the part that was missing until 2026-09-18 -- an actual
// HARDWARE watchdog feed between sectors.
//
// docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md is the diagnosis
// this shape implements. The two things it established that the previous
// version of this comment got wrong:
//
//  1. watchdog_task_checkin() is BOOKKEEPING ONLY. It writes a tick into
//     watchdog_task.c's s_last_checkin_tick[] and touches no hardware. The
//     bracketing check-ins therefore only ensure update_task is not BLAMED by
//     the software gate; they never reset the hardware timer. The old comment
//     here claimed block-at-a-time was chosen so "the watchdog stays fed
//     across the whole operation" -- splitting the work bounds each stall,
//     but nothing in the loop was feeding anything.
//  2. The one hardware feed in this firmware lives in watchdog_task, which is
//     pinned to SAFTYFW_CORE_TRIP_PATH and provably cannot be scheduled while
//     hal_flash_safe_execute() is running: the pico-sdk FreeRTOS-SMP lockout
//     path spawns a configMAX_PRIORITIES-1 task on the OTHER core (i.e. core
//     1, watchdog_task's core) that disables interrupts and spins, then
//     disables interrupts on this core too. So the feed has to be issued from
//     HERE, by the task doing the erasing, or it does not happen at all.
//
// Hence watchdog_task_feed_if_all_within_deadline() below. It routes through
// the SAME gate watchdog_task's own loop uses, so this moves only WHO owns
// the feed, never the POLICY of when one is allowed: if any registered task
// is past its own deadline, no feed is issued and the watchdog reboots the
// chip exactly as it would have. A genuinely wedged safety processor is still
// rebooted mid-update; only a HEALTHY one stops being reset for the crime of
// erasing flash. update_task runs at the lowest priority on core 0
// (task_priorities.h), which makes this placement honest in practice too:
// every higher-priority task on both cores has had its chance to run and
// check in by the time this line is reached after a lockout ends.
//
// Cost of the 64K -> 4K change: ~208 sector erases instead of 13 block
// erases, trading total wall-clock erase time (roughly 2 s to roughly 9 s at
// typical per-sector times) for a bounded per-stall time that fits inside
// SAFTYFW_WATCHDOG_TIMEOUT_MS. That is the right trade on a safety processor,
// and it is why KilnFW's RELAY_ERASE_TIMEOUT_MS (ota_pico_relay.c) moved with
// it. BOOTLOADER_SLOT_FLASH_SIZE (0xD0000, 851968 bytes) is exactly 208 * 4K
// with no remainder, so there is no short tail chunk here to get wrong -- the
// _Static_assert on UPDATE_TASK_ERASE_CHUNK_SIZE enforces that, and
// update_erase_plan_chunk() handles a short tail correctly anyway if a future
// slot size ever stops dividing evenly.
static bool update_task_erase_slot(uint32_t slot_offset)
{
    if (!update_task_ensure_flash_region()) {
        return false;
    }

    // Assert-like guard (2026-09-21 triage finding): never erase a slot that
    // would touch this board's own running image. Checked against the WHOLE
    // slot up front, before the first chunk, rather than per-chunk -- the
    // hazard is "this operation must never start", not "stop partway".
    if (update_task_region_overlaps_running_image(slot_offset, BOOTLOADER_SLOT_FLASH_SIZE)) {
        log_task_log(LOG_LEVEL_ERROR, "update",
                     "erase_slot: refused -- target slot overlaps running image");
        return false;
    }

    const uint32_t chunk_count =
        update_erase_plan_chunk_count(BOOTLOADER_SLOT_FLASH_SIZE, UPDATE_TASK_ERASE_CHUNK_SIZE);

    for (uint32_t i = 0; i < chunk_count; i++) {
        uint32_t offset = 0;
        uint32_t len = 0;
        if (!update_erase_plan_chunk(slot_offset, BOOTLOADER_SLOT_FLASH_SIZE,
                                      UPDATE_TASK_ERASE_CHUNK_SIZE, i, &offset, &len)) {
            return false; // cannot happen for i < chunk_count; fail closed rather than erase a guessed range
        }

        watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);
        update_erase_args_t args = { .offset = offset, .count = len, .result = HAL_NOT_READY };
        hal_status_t status = hal_flash_safe_execute(update_erase_cb, &args, 2000u);
        watchdog_task_checkin(WATCHDOG_CHECKIN_UPDATE_TASK);

        if (status != HAL_OK || args.result != HAL_OK) {
            return false;
        }

        // The hardware feed, gated exactly as watchdog_task would gate it.
        // Deliberately AFTER the post-erase checkin above, so this task's own
        // entry is fresh when the gate evaluates it, and deliberately not
        // conditional on anything here -- the gate itself is the only thing
        // allowed to decide whether a feed happens.
        (void)watchdog_task_feed_if_all_within_deadline();
    }

    return true;
}

// --- Flash program (UPDATE_DATA) -------------------------------------------

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t count;
    hal_status_t result; // 2026-09-06 review fix -- see update_metadata_write_args_t's
                          // own result field comment for why this must be
                          // checked, not discarded.
} update_program_args_t;

static void update_program_cb(void *param)
{
    update_program_args_t *a = (update_program_args_t *)param;
    a->result = hal_flash_program(&s_flash_region, a->offset, a->data, a->count);
}

// See this file's header comment ("The 248-byte chunk / 256-byte flash page
// mismatch") for the read-modify-write reasoning. `rel_offset`/`len` are
// slot-relative and already validated by update_received_ranges_mark()
// before this is called.
static bool update_task_program_chunk(uint32_t slot_flash_offset, uint32_t rel_offset,
                                       const uint8_t *data, size_t len)
{
    if (!update_task_ensure_flash_region()) {
        return false;
    }

    uint32_t abs_start = slot_flash_offset + rel_offset;
    uint32_t first_page = abs_start & ~(uint32_t)(HAL_FLASH_PROGRAM_SIZE - 1u);
    uint32_t last_byte = abs_start + (uint32_t)len - 1u;
    uint32_t last_page = last_byte & ~(uint32_t)(HAL_FLASH_PROGRAM_SIZE - 1u);
    uint32_t page_count = ((last_page - first_page) / HAL_FLASH_PROGRAM_SIZE) + 1u;

    // UPDATE_CHUNK_LEN (248) < 2 * HAL_FLASH_PROGRAM_SIZE (512), so a chunk
    // can never legitimately span more than two pages -- defensive check
    // kept anyway since `len` ultimately comes off the wire.
    if (page_count > 2u) {
        return false;
    }

    uint8_t page_buf[2u * HAL_FLASH_PROGRAM_SIZE];
    if (hal_flash_read(&s_flash_region, first_page, page_buf,
                        (size_t)page_count * HAL_FLASH_PROGRAM_SIZE) != HAL_OK) {
        return false;
    }

    uint32_t buf_offset = abs_start - first_page;
    memcpy(page_buf + buf_offset, data, len);

    update_program_args_t args = {
        .offset = first_page,
        .data = page_buf,
        .count = (size_t)page_count * HAL_FLASH_PROGRAM_SIZE,
        .result = HAL_NOT_READY,
    };
    hal_status_t status = hal_flash_safe_execute(update_program_cb, &args, 1000u);
    return status == HAL_OK && args.result == HAL_OK;
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
    // Best-effort -- transfer is being torn down regardless -- but a failure
    // here previously vanished with no trace anywhere; log it so a stuck
    // STAGED/PENDING_VERIFY slot metadata record left behind by a failed
    // revert is not a silent mystery later.
    if (!update_task_persist_metadata(&meta, latest)) {
        log_task_log(LOG_LEVEL_WARN, "update", "revert_target_slot: metadata persist failed");
    }

    s_transfer_active = false;
}

// --- UPDATE_BEGIN ------------------------------------------------------------

static void update_task_gather_preconditions(update_preconditions_t *out)
{
    bool relay_energized = false, heating_enabled = false;
    safety_core_get_output_status(&relay_energized, &heating_enabled);
    out->relay_open = !relay_energized;

    safety_trip_t trip_reason = SAFETY_TRIP_NONE;
    uint8_t diag_state = 0;
    /* opus review finding (LOW): warn_active used to be gathered into a
     * local that nothing downstream ever read -- this function only needs
     * trip_reason, so pass NULL rather than carry a dead variable. */
    safety_core_get_diag_status(&trip_reason, NULL, &diag_state, NULL);
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

    // Running-image flash-overlap guard (2026-09-21 triage finding), checked
    // HERE -- before announcing ERASING at all -- so a board in this state
    // gets the specific, descriptive refusal below rather than reaching
    // update_task_erase_slot()'s own defence-in-depth copy of this same
    // check (still present there and in update_task_persist_metadata(), see
    // both functions' own comments) and only being reported generically as
    // UPDATE_STATUS_ERR_INTERNAL. Checks both regions this flow is about to
    // touch: the target slot (about to be erased) and the metadata sector
    // (about to be programmed once STAGED metadata is persisted below).
    if (update_task_region_overlaps_running_image(s_slot_flash_offset, BOOTLOADER_SLOT_FLASH_SIZE) ||
        update_task_region_overlaps_running_image(BOOTLOADER_METADATA_FLASH_OFFSET,
                                                    BOOTLOADER_METADATA_FLASH_SIZE)) {
        log_task_log(LOG_LEVEL_ERROR, "update",
                     "begin: refused -- this board's running image is flat/bootloader-less and "
                     "overlaps the metadata sector or the target slot; refusing rather than risk "
                     "corrupting the running image (2026-09-21 triage finding)");
        update_task_send_status_now(UPDATE_TASK_STATE_REFUSED_RUNNING_IMAGE_OVERLAP, UPDATE_STATUS_ERR_INTERNAL);
        return;
    }

    s_header = hdr;
    s_sticky_error = 0;

    update_task_send_status_now(UPDATE_TASK_STATE_ERASING, 0);

    if (!update_task_erase_slot(s_slot_flash_offset)) {
        log_task_log(LOG_LEVEL_ERROR, "update", "begin: erase_slot failed");
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
        log_task_log(LOG_LEVEL_ERROR, "update", "begin: STAGED metadata persist failed");
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

// --- Slot-linkage validation (owner decision 2026-09-20) --------------------
//
// bootloader/main.c's jump_to_app() jumps to whatever vector table sits at
// the ACTIVE slot's start, unconditionally: word 0 loads straight into SP,
// word 1 straight into PC (msr msp / bx). Nothing there checks those two
// words are plausible for the slot they're in -- only that the full image
// CRCs correctly against a checksum the ESP itself supplied for whatever
// bytes it sent. That means an image linked for slot A but written into
// slot B (a build/tooling mixup, not a transmission error -- the two slot
// executables are the SAME sources at two different link origins, see
// CMakeLists.txt's saftyfw_add_slot_executable()) sails through CRC
// verification unchanged (the bytes received are exactly the bytes sent,
// uncorrupted) and would still get jumped into with a stack pointer and
// reset vector baked in for the WRONG absolute address. Depending on what
// actually sits at the mismatched offset in the other slot's image, that is
// not necessarily a hard fault the bootloader's fallback (main.c's 2-attempt
// CRC retry, which only re-checks the SAME already-corrupt-relative-to-slot
// image and would not catch this) or anything downstream would notice --
// corrupt-code execution accepted as legitimate, on the SAFETY processor.
//
// This function catches exactly that build/tooling-mixup shape -- an
// otherwise well-formed image linked for one slot's address, landed intact
// in the OTHER slot -- before update_task_process_end() ever flips
// meta.active_slot to point at it. It is a plausibility check, not a
// signature check (docs/BOOTLOADER.md section 6, signing, stays unpicked),
// against the two facts every valid Cortex-M image for this bootloader must
// satisfy:
//
//   1. Initial SP (word 0) lies inside RP2040 SRAM -- SRAM_BASE (0x20000000)
//      through SRAM_END (0x20042000), pico-sdk's own hardware/regs/
//      addressmap.h constant values, restated below as local literals (see
//      that #define block) rather than included from hardware/regs/
//      addressmap.h itself, since check_hal_include_boundary.ps1 does not
//      allow a new hardware/ include in this file outside the existing
//      HAL-routed set. Checked against SRAM_BASE/SRAM_END rather
//      than an independently-chosen range because those two constants
//      together span EXACTLY the RAM + SCRATCH_X + SCRATCH_Y regions
//      bootloader/app_slot.ld.in's linker script carves out of SRAM for
//      this application (RAM: 0x20000000..0x20040000, 256K; SCRATCH_X:
//      0x20040000..0x20041000; SCRATCH_Y: 0x20041000..0x20042000 == SRAM_END)
//      -- confirmed by reading that file, not assumed.
//   2. Reset vector (word 1) has the Thumb bit set (bit 0 -- required for
//      every Cortex-M instruction address; this MCU has no ARM mode) AND
//      falls inside the TARGET slot's own flash window, i.e.
//      [XIP_BASE + target_slot_offset, XIP_BASE + target_slot_offset +
//      BOOTLOADER_SLOT_FLASH_SIZE) -- the slot the metadata flip is about to
//      make active, not merely "some slot". A reset vector that decodes
//      fine but points into the OTHER slot's flash window is exactly the
//      wrong-slot-image signature this check exists to catch.
//
// Only used from update_task_process_end() below, against an INCOMING image
// immediately after its CRC has already been confirmed and before it is
// made active -- not reused for the currently-running application's own
// self-check at boot (that is a different problem: this bootloader's own
// jump_to_app() has no equivalent guard today, see the coordinator's task
// brief on bootloader/main.c's fallback behavior).
// The actual comparison arithmetic lives in update_task_slot_linkage.c/.h
// (pure, no pico-sdk/FreeRTOS dependency, host-tested there -- see that
// module's own header comment) -- this wrapper's only job is reading the
// two vector-table words out of the mapped flash region and supplying the
// SRAM_BASE/SRAM_END/XIP_BASE constants that module cannot see for itself.
//
// These three are restated as local literals rather than pulled from
// pico-sdk's hardware/regs/addressmap.h: that header lives under hardware/
// and tools/check_hal_include_boundary.ps1 refuses a new hardware/ include
// in this file outside the existing HAL-routed set (hal_flash.h/hal_wdt.h)
// without an allowlist entry, and these three values are architectural
// constants of the RP2040 itself (fixed for every RP2040 chip, not a board-
// or SDK-version-specific detail) -- SRAM_BASE/SRAM_END confirmed against
// bootloader/app_slot.ld.in's linker script (see the header comment above),
// XIP_BASE confirmed against flash_layout.h's own offsets, which are already
// expressed relative to it.
#define UPDATE_TASK_SLOT_LINKAGE_SRAM_BASE 0x20000000u
#define UPDATE_TASK_SLOT_LINKAGE_SRAM_END  0x20042000u
#define UPDATE_TASK_SLOT_LINKAGE_XIP_BASE  0x10000000u

static bool update_task_slot_linkage_plausible(const void *slot_data_ptr, uint32_t target_slot_offset,
                                                uint32_t image_length)
{
    // image_length guard: the two vector-table words read below are the
    // first 8 bytes of the mapped region. update_image_header_validate()
    // (image_header.c) only rejects length == 0 and length > max_length --
    // an image as short as a few bytes (e.g. length == 4) passes that check
    // and would otherwise let this read run past the end of a region only
    // mapped for image_length bytes. Reject before touching vectors[0..1].
    if (image_length < 8u) {
        return false;
    }

    const uint32_t *vectors = (const uint32_t *)slot_data_ptr;
    uint32_t sp = vectors[0];
    uint32_t reset_vector = vectors[1];

    return update_task_slot_linkage_check(sp, reset_vector, image_length, target_slot_offset,
                                           BOOTLOADER_SLOT_FLASH_SIZE,
                                           UPDATE_TASK_SLOT_LINKAGE_SRAM_BASE,
                                           UPDATE_TASK_SLOT_LINKAGE_SRAM_END,
                                           UPDATE_TASK_SLOT_LINKAGE_XIP_BASE);
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
    if (!update_task_ensure_flash_region()) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_INTERNAL);
        update_task_revert_target_slot();
        return;
    }
    const void *slot_data_ptr = NULL;
    if (hal_flash_map(&s_flash_region, s_slot_flash_offset, s_header.length, &slot_data_ptr) !=
        HAL_OK) {
        update_task_send_status_now(UPDATE_TASK_STATE_FAILED, UPDATE_STATUS_ERR_INTERNAL);
        update_task_revert_target_slot();
        return;
    }
    uint32_t actual_crc = bootloader_crc32((const uint8_t *)slot_data_ptr, s_header.length);

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

    // Slot-linkage plausibility check -- see
    // update_task_slot_linkage_plausible()'s header comment above. Runs
    // AFTER the CRC check (so a genuinely corrupted transfer is still
    // reported as CRC_MISMATCH, not this) and BEFORE the metadata flip
    // below that would make this slot active.
    if (!update_task_slot_linkage_plausible(slot_data_ptr, s_slot_flash_offset, s_header.length)) {
        log_task_log(LOG_LEVEL_ERROR, "update",
                     "end: slot linkage implausible (SP/reset vector do not fit target slot -- wrong-slot image?)");
        update_task_send_status_now(UPDATE_TASK_STATE_REJECTED_SLOT_LINKAGE, UPDATE_STATUS_ERR_CRC_MISMATCH);
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
        log_task_log(LOG_LEVEL_ERROR, "update", "end: PENDING_VERIFY metadata persist failed");
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
    // Populates update_task_get_active_slot()'s cache from this same read,
    // unconditionally -- independent of the PENDING_VERIFY branch below,
    // which is s_own_slot's own narrower concern (see that variable's
    // comment). A real, well-formed metadata record always names an active
    // slot, whether or not this boot happens to be mid-confirm.
    // Store order matters: link_task (higher priority) can preempt this
    // task between these two statements. Store s_active_slot FIRST, then a
    // compiler barrier, then s_active_slot_known LAST -- so any reader that
    // observes KNOWN == true is guaranteed to also see the already-stored,
    // correct s_active_slot value (never a stale/default slot alongside a
    // true KNOWN flag).
    s_active_slot = meta.active_slot;
    __compiler_memory_barrier();
    s_active_slot_known = true;
    if (meta.slots[meta.active_slot].state == BOOTLOADER_SLOT_PENDING_VERIFY) {
        s_confirm_pending = true;
        s_own_slot = meta.active_slot;
    }
}

// Gathers real evidence (src/update/confirm.h's update_confirm_checklist_t)
// and, once update_confirm_missing() returns 0, marks this slot VALID
// exactly once.
//
// config_crc_ok is now wired to config_store's real cache (TODO.md Phase
// 10.8's "remaining half", closed by this pass): config_store_get_config_
// version() reads the cached record's seq (truncated to u8), and
// config_store_confirm_crc_ok() (config_store.c, pure/host-tested) applies
// the safe-direction rule documented on its own declaration in
// config_store.h -- version 0 means "no CRC-verified record has ever been
// loaded" (either config_store_boot_load() has not run, or it ran and found
// nothing valid in flash), and that must gate CLOSED, never open. Getting
// this backwards -- letting an unreadable/uninitialised config store read as
// "confirmed good" -- would let a freshly-flashed image reach
// BOOTLOADER_SLOT_VALID without its config ever actually having been
// verified, which is exactly the "sails through any check that just proves
// main() ran" failure docs/BOOTLOADER.md section 5 warns against. A slot
// stuck at PENDING_VERIFY is recoverable; a slot wrongly marked VALID is not
// (short of another update), so "stuck closed" is the only defensible
// default here.
static void update_task_confirm_tick(void)
{
    if (!s_confirm_pending) {
        return;
    }

    update_confirm_checklist_t c;
    c.config_crc_ok = config_store_confirm_crc_ok(config_store_get_config_version());

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
    } else {
        // s_confirm_pending stays true and this retries on the next
        // UPDATE_CONFIRM_TICK_PERIOD_MS tick -- but a persistently failing
        // flash write here would otherwise retry forever with zero trace.
        log_task_log(LOG_LEVEL_WARN, "update", "confirm_tick: VALID metadata persist failed, will retry");
    }
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
bool update_task_request_rollback(const char **out_reason, uint8_t *out_reason_code)
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
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_ARMED;
        }
        return false;
    }

    bootloader_metadata_t meta;
    size_t latest = update_task_read_latest_metadata(&meta);
    if (latest == BOOTLOADER_METADATA_NO_SLOT) {
        if (out_reason) {
            *out_reason = "refused: no bootloader metadata to roll back from";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_NO_METADATA;
        }
        return false;
    }
    if (meta.active_slot >= BOOTLOADER_SLOT_COUNT) {
        if (out_reason) {
            *out_reason = "refused: malformed metadata (active_slot out of range)";
        }
        if (out_reason_code) {
            // Not one of the three "expected" refusal shapes -- malformed
            // metadata should not occur in practice, same "should not occur"
            // role KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN documents.
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_UNKNOWN;
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
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_SLOT_INVALID;
        }
        return false;
    }

    if (!update_task_persist_metadata(&decision.updated_meta, latest)) {
        if (out_reason) {
            *out_reason = "flash write failed";
        }
        if (out_reason_code) {
            *out_reason_code = KILNLINK_ROLLBACK_RESULT_REASON_STORAGE;
        }
        return false;
    }

    if (out_reason) {
        *out_reason = "ok";
    }
    // out_reason_code is deliberately left untouched here: this function
    // never returns on success (watchdog_reboot() below), so there is no
    // path from here that could carry a "success" reason code anywhere --
    // see kilnlink_rollback_result.h's own "ASYMMETRIC BY DESIGN" comment.

    // No further code in this function runs after this call -- the caller
    // must log "accepted" (or otherwise act on `out_reason == "ok"`) BEFORE
    // calling this function's caller chain concludes, since hal_wdt_reboot()
    // resets the RP2040 immediately rather than returning. hal_wdt_reboot()
    // is exactly this call site per hal_wdt.h's own header comment
    // ("Pico watchdog_reboot() -- update_task.c:998, SaftyFW's sole call
    // site, always as watchdog_reboot(0, 0, 0)") -- backend exists
    // (pico/wdt/hal_wdt_pico.c) and is now wired into hwabstraction_pico.
    hal_wdt_reboot();
    for (;;) {
        // Defensive only: hal_wdt_reboot() does not return on real
        // hardware. Never reached, but a function declared to return bool
        // must not fall off its own end.
    }
}

// --- Reboot in place (SAFETY_CMD_REBOOT, 0x29) ----------------------------
//
// The Pico half of KilnFW's POST /api/sw_reset. Split policy/act pair --
// see update_task.h's own doc comments on both functions for why this one
// is split where update_task_request_rollback() above is not.

bool update_task_reboot_allowed(const char **out_reason, uint8_t *out_reason_code)
{
    // Gathers the two live inputs and hands them to the pure decision table
    // in update_task_reboot_policy.c -- 2026-09-09, split out so the
    // decision itself can be host-compiled and behaviourally tested
    // (test_update_task_reboot_policy.c), rather than only pinned by
    // test_reboot_in_place_wiring.c's source-text scan (which proves the
    // right identifiers are PRESENT, not that the function returns the
    // right thing). See update_task_reboot_policy.h for the full rationale
    // and the ARMED-checked-first ordering.
    //
    // relay_energized: the SAME ARMED-equivalent gate
    // update_task_request_rollback() above uses, reached the same legal way
    // (safety_core_get_output_status(), not relay_owner.h directly -- see
    // this file's header comment). A reboot in place is less disruptive
    // than a rollback in that it changes no image and no configuration, but
    // it is exactly as disruptive in the one way that matters here: the
    // safety processor stops supervising for a couple of seconds. That is
    // not something to do while it is holding heating permission.
    bool relay_energized = false;
    safety_core_get_output_status(&relay_energized, NULL);

    // transfer_active: a firmware transfer into the inactive slot is in
    // flight. The ESP's own ota_http_check_interlocks() already refuses
    // this case (snap.other_update_in_progress), so on the normal path this
    // is defence in depth -- but the Pico-side policy is the half that must
    // be independently correct, and OTHER senders reach it with no such
    // check at all: the raw uart_bridge passthrough, and any bench tool
    // that can put a 0x29 on the wire.
    //
    // What rebooting here would have cost: s_transfer_active spans
    // UPDATE_BEGIN..UPDATE_END, during which chunks are being written into
    // the inactive slot and the receive/gap-tracking state lives only in
    // RAM. A watchdog_reboot() mid-transfer would leave that slot holding a
    // partial image with no record that it is partial, and would discard the
    // gap cursor, so the ESP's transfer would fail (or, worse, have to be
    // restarted blind) against a slot whose contents nobody can describe.
    // Refusing costs the operator one retry after the update finishes;
    // accepting costs a half-written firmware slot.
    return update_task_reboot_policy_decide(relay_energized, update_task_transfer_active(),
                                            out_reason, out_reason_code);
}

void update_task_reboot_now(void)
{
    // Everything this function does is on these four lines, and that is the
    // point: no update_task_persist_metadata(), no config_store_* call, no
    // flash_safe_execute(), no bootloader_decide_* -- nothing that could
    // change which slot boots or what configuration it boots with. Compare
    // update_task_request_rollback() above, which deliberately DOES write a
    // metadata record before its own hal_wdt_reboot(). If a future edit adds
    // any write to this function, test_reboot_in_place_wiring.c (SaftyFW's
    // host test suite) fails: it scans this exact function body.
    log_task_log(LOG_LEVEL_WARN, "reboot", "rebooting in place, same slot, no config touched");

    // Same single reboot mechanism the rest of this file already uses --
    // hal_wdt.h's Pico backend is pico-sdk's watchdog_reboot(0, 0, 0). No
    // second mechanism is introduced for this command.
    hal_wdt_reboot();
    for (;;) {
        // Defensive only: hal_wdt_reboot() does not return on real hardware.
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

bool update_task_transfer_active(void)
{
    return s_transfer_active;
}

bool update_task_get_active_slot(bool *out_is_b)
{
    if (!s_active_slot_known) {
        return false;
    }
    if (out_is_b != NULL) {
        *out_is_b = (s_active_slot == BOOTLOADER_SLOT_B);
    }
    return true;
}

bool update_task_start(void)
{
    s_transfer_active = false;
    s_confirm_pending = false;
    s_own_slot = BOOTLOADER_SLOT_A;
    s_active_slot_known = false;
    s_active_slot = BOOTLOADER_SLOT_A;
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
