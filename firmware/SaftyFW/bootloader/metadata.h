// metadata.h -- the bootloader's flash metadata record and boot-selection
// logic. docs/BOOTLOADER.md sections 2 ("Metadata") and 3 ("What the
// bootloader does").
//
// Deliberately pure: no RTOS, no pico-sdk, no direct flash I/O. Every
// function here operates on caller-supplied buffers/structs, the same
// discipline src/tasks/link_frame.c and src/safety_guards.c already use for
// the same reason -- this is the logic that decides which of two images
// runs, and it needs to be provably correct against synthetic inputs before
// it ever touches real flash. bootloader/main.c is the thin, RTOS-free,
// SDK-coupled caller that does the actual flash_range_erase()/
// flash_range_program()/XIP-memory-mapped reads and hands buffers in here.
//
// --- Why this is a log, not the two-copies-ping-pong docs/BOOTLOADER.md
// originally described ---
//
// The original section 2 text said: "Two copies in the metadata sector,
// each CRC'd, written alternately so a power loss during a metadata write
// always leaves one valid copy." That describes writing copy A, then copy
// B, then A again, forever, within the SAME 4K erase sector
// (flash_layout.h's BOOTLOADER_METADATA_FLASH_SIZE is one sector, not two).
//
// That scheme is not actually achievable on NOR flash. A flash bit can only
// go 1 -> 0 without an erase; writing a *changed* value into a location that
// already holds different bits requires erasing first, and RP2040's W25Q-
// class flash can only erase in whole 4K-sector units. So "write copy B"
// after "copy A already occupies part of this sector" cannot merely write
// B's bytes -- if B's location already holds old data, it must be erased
// first, and erasing the sector destroys copy A's bytes too, in the same
// operation. The "always one valid copy" guarantee breaks on the very first
// update, not eventually.
//
// The fix used here keeps the exact same offset and size
// (flash_layout.h is UNCHANGED) and replaces "two copies" with a small
// append-only log: BOOTLOADER_METADATA_SLOTS_PER_SECTOR fixed-size records
// per sector, written to the next free (erased, 0xFF) slot rather than
// overwriting one in place. A power loss mid-write leaves a corrupt record
// at the write-in-progress slot (caught by its own CRC and discarded) while
// every previously-written record is untouched -- the same "always one good
// copy" property the original text wanted, but achieved without ever
// requiring an erase except when the sector is genuinely full. Finding the
// current metadata is "scan every slot, keep the one with the highest `seq`
// among those whose own CRC validates" -- bootloader_metadata_find_latest()
// below.
#ifndef SAFTYFW_BOOTLOADER_METADATA_H
#define SAFTYFW_BOOTLOADER_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

// One flash-programming page (RP2040 minimum program granularity), and the
// natural size for one metadata record -- page-aligned writes are the
// simplest correct thing on NOR flash, and 256 bytes is far more than this
// record needs (see BOOTLOADER_METADATA_RECORD_USED_LEN below).
#define BOOTLOADER_METADATA_RECORD_LEN 256u
#define BOOTLOADER_METADATA_SLOTS_PER_SECTOR \
    (BOOTLOADER_METADATA_FLASH_SIZE / BOOTLOADER_METADATA_RECORD_LEN) // 16

// Sentinel: no record slot chosen / nothing found.
#define BOOTLOADER_METADATA_NO_SLOT ((size_t)-1)

#define BOOTLOADER_METADATA_MAGIC   0x4B4C4E31u // "KLN1", distinct from any
                                                 // kilnlink/link_frame magic
#define BOOTLOADER_METADATA_FORMAT_VERSION 1u

// slot_state_t -- docs/BOOTLOADER.md section 2's table, one per application
// slot (A or B).
typedef enum {
    BOOTLOADER_SLOT_EMPTY = 0,
    BOOTLOADER_SLOT_STAGED = 1,
    BOOTLOADER_SLOT_VALID = 2,
    BOOTLOADER_SLOT_PENDING_VERIFY = 3,
    BOOTLOADER_SLOT_BAD = 4,
} bootloader_slot_state_t;

// Which of the two application slots -- also used as an array index
// (bootloader_metadata_t.slots[BOOTLOADER_SLOT_A] etc), so these values are
// load-bearing, not just labels.
#define BOOTLOADER_SLOT_A 0u
#define BOOTLOADER_SLOT_B 1u
#define BOOTLOADER_SLOT_COUNT 2u

// version/build_commit are ASCII, NOT null-terminated on the wire/in-flash
// (same convention as CommonFW/docs/LINK_PROTOCOL.md's FW_VERSION frame) --
// callers that want a C string must copy and terminate themselves.
typedef struct {
    bootloader_slot_state_t state;
    uint32_t length;   // bytes actually written, checked against a fresh CRC every boot
    uint32_t crc32;    // over [0, length) of this slot's flash region
    char     version[16];
    char     build_commit[20];
    uint32_t build_epoch; // 0 = unknown, matching this codebase's "unknown maps to
                           // the honest-but-uninformative value" convention
} bootloader_slot_meta_t;

typedef struct {
    uint16_t format_version;
    uint32_t seq; // log sequence number; bootloader_metadata_find_latest() keeps the highest
    uint8_t  active_slot; // BOOTLOADER_SLOT_A or _B
    uint8_t  boot_attempts;
    bootloader_slot_meta_t slots[BOOTLOADER_SLOT_COUNT];
} bootloader_metadata_t;

// Above which boot_attempts on the active slot triggers a fallback
// (docs/BOOTLOADER.md section 3 step 3).
#define BOOTLOADER_MAX_BOOT_ATTEMPTS 3u

// --- Record pack/unpack -------------------------------------------------

// Packs `meta` into a BOOTLOADER_METADATA_RECORD_LEN-byte record, including
// computing and writing the trailing record_crc32 over everything before it.
// Pure byte layout, no validation of `meta`'s own field values (that is the
// caller's job before calling this, same "this function does not decide
// what any field means" discipline as link_frame.c's pack functions).
void bootloader_metadata_pack(const bootloader_metadata_t *meta,
                               uint8_t out[BOOTLOADER_METADATA_RECORD_LEN]);

// Unpacks and validates a single record: magic, format_version, and the
// trailing CRC must all check out. Returns false (and leaves `*out`
// completely untouched) on ANY of: NULL args, wrong magic, unrecognised
// format_version, or a CRC mismatch (the record was corrupted, or `in` is
// erased/all-0xFF flash, or `in` is a still-in-progress torn write). This
// is untrusted input in the same sense wire frames are -- it is read back
// from flash, which degrades and which a power loss can leave mid-write --
// so the same "validate everything before touching *out" discipline
// link_frame_unpack_context() uses applies here too.
bool bootloader_metadata_unpack(const uint8_t in[BOOTLOADER_METADATA_RECORD_LEN],
                                 bootloader_metadata_t *out);

// --- Sector scan ---------------------------------------------------------

// Scans all BOOTLOADER_METADATA_SLOTS_PER_SECTOR records in `sector`
// (exactly BOOTLOADER_METADATA_FLASH_SIZE bytes, as read straight from the
// XIP-mapped metadata region -- this function does no flash I/O itself),
// unpacks each, and keeps the one with the highest `seq` among those that
// unpack successfully. Corrupt or erased (all-0xFF) slots are silently
// skipped, not treated as errors -- an erased slot is the expected steady
// state for every slot the log has not reached yet.
//
// Returns the winning slot's index (0..BOOTLOADER_METADATA_SLOTS_PER_SECTOR-1)
// and fills `*out_meta` with its contents, or BOOTLOADER_METADATA_NO_SLOT
// (leaving `*out_meta` untouched) if not a single slot in the sector holds a
// valid record -- docs/BOOTLOADER.md section 3 step 2, "if both copies are
// bad, enter recovery", generalised to "if no record in the log is good".
size_t bootloader_metadata_find_latest(const uint8_t sector[BOOTLOADER_METADATA_FLASH_SIZE],
                                        bootloader_metadata_t *out_meta);

// Given the slot index bootloader_metadata_find_latest() returned (or
// BOOTLOADER_METADATA_NO_SLOT if the sector has never held a valid record),
// returns the index the NEXT write should target. Wraps to 0 when the
// sector is full (latest_slot_index == BOOTLOADER_METADATA_SLOTS_PER_SECTOR - 1)
// or when there was no previous valid record at all -- both cases mean
// "start from slot 0", but the caller must erase the whole sector first in
// the wrap case (there is old data occupying slot 0 already) and must NOT
// erase in the never-written case (slot 0 might already be usably erased,
// and erasing unnecessarily is exactly the kind of extra flash-wear/extra-
// window-for-a-power-loss this scheme exists to minimise). Use
// bootloader_metadata_next_write_needs_erase() to tell the two apart.
size_t bootloader_metadata_next_write_slot(size_t latest_slot_index);

// True iff the write slot bootloader_metadata_next_write_slot() just
// returned requires a sector erase first -- i.e. the log was full and wrapped,
// as opposed to this being the sector's first-ever write.
bool bootloader_metadata_next_write_needs_erase(size_t latest_slot_index);

// --- Boot decision ---------------------------------------------------------

typedef struct {
    bool    bootable;              // false => caller must enter recovery
    uint8_t chosen_slot;           // BOOTLOADER_SLOT_A/_B, meaningful only if bootable
    bool    needs_metadata_update; // true => caller must persist updated_meta before jumping
    bootloader_metadata_t updated_meta;
} bootloader_boot_decision_t;

// docs/BOOTLOADER.md section 3, steps 2-3: given the metadata record just
// read from flash, decides which slot (if any) the caller should attempt to
// boot, applying the boot_attempts limit and falling back to the other slot
// when the active one is not in a bootable state. Does NOT CRC-check the
// application image itself (that needs real flash reads over up to 832K,
// which is main.c's job using bootloader_crc32() from crc32.h) -- this
// function only reasons about the metadata record's own fields.
//
// A slot counts as bootable if its state is VALID or PENDING_VERIFY
// (docs/BOOTLOADER.md section 5: a freshly-updated slot boots as
// PENDING_VERIFY precisely so the application gets a chance to confirm
// itself -- a bootloader that refused to boot a PENDING_VERIFY slot would
// make rollback-on-failure-to-confirm impossible, since the app would never
// run at all).
//
// Pure: does not touch flash, does not mutate `*meta` -- the decision to
// persist anything is communicated via `needs_metadata_update`/
// `updated_meta`, and it is main.c's job to actually write it.
bootloader_boot_decision_t bootloader_decide_boot(const bootloader_metadata_t *meta);

// docs/BOOTLOADER.md section 3 step 4's "every boot, not just the first
// after an update" CRC check happens in main.c (it needs real flash I/O);
// when that check fails against the slot bootloader_decide_boot() chose,
// main.c calls this to get the fallback decision: `failed_slot` is marked
// BAD, and the other slot is tried if it is VALID or PENDING_VERIFY, exactly
// mirroring bootloader_decide_boot()'s own fallback logic (a slot that fails
// a fresh CRC check is exactly as unbootable as one whose metadata already
// said BAD/EMPTY/STAGED -- this function exists so that fact does not have
// to be re-derived at the call site).
bootloader_boot_decision_t bootloader_decide_after_crc_fail(const bootloader_metadata_t *meta,
                                                              uint8_t failed_slot);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOTLOADER_METADATA_H
