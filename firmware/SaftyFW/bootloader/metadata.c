// metadata.c -- see metadata.h.
#include "metadata.h"

#include <string.h>

#include "crc32.h"

// --- Byte layout (little-endian, same convention as src/tasks/link_frame.c)
//
// Offset  Size  Field
//      0     4  magic
//      4     2  format_version
//      6     2  reserved0 (0)
//      8     4  seq
//     12     1  active_slot
//     13     1  boot_attempts
//     14     2  reserved1 (0)
//     16    52  slots[BOOTLOADER_SLOT_A]  (see SLOT_BLOCK_LEN layout below)
//     68    52  slots[BOOTLOADER_SLOT_B]
//    120   132  reserved, 0xFF-filled
//    252     4  record_crc32, over bytes [0, 252)
//    256  total = BOOTLOADER_METADATA_RECORD_LEN
#define REC_OFF_MAGIC          0u
#define REC_OFF_FORMAT_VERSION 4u
#define REC_OFF_SEQ            8u
#define REC_OFF_ACTIVE_SLOT    12u
#define REC_OFF_BOOT_ATTEMPTS  13u
#define REC_OFF_SLOTS          16u
#define REC_OFF_CRC            252u

// Per-slot block (52 bytes):
//   0  1  state
//   1  3  reserved (0)
//   4  4  length
//   8  4  crc32
//  12 16  version (ASCII, not null-terminated)
//  28 20  build_commit (ASCII, not null-terminated)
//  48  4  build_epoch
#define SLOT_BLOCK_LEN       52u
#define SLOT_OFF_STATE       0u
#define SLOT_OFF_LENGTH      4u
#define SLOT_OFF_CRC32       8u
#define SLOT_OFF_VERSION     12u
#define SLOT_OFF_BUILD_COMMIT 28u
#define SLOT_OFF_BUILD_EPOCH 48u

static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static void pack_slot(const bootloader_slot_meta_t *slot, uint8_t out[SLOT_BLOCK_LEN])
{
    memset(out, 0, SLOT_BLOCK_LEN);
    out[SLOT_OFF_STATE] = (uint8_t)slot->state;
    put_u32_le(&out[SLOT_OFF_LENGTH], slot->length);
    put_u32_le(&out[SLOT_OFF_CRC32], slot->crc32);
    memcpy(&out[SLOT_OFF_VERSION], slot->version, sizeof(slot->version));
    memcpy(&out[SLOT_OFF_BUILD_COMMIT], slot->build_commit, sizeof(slot->build_commit));
    put_u32_le(&out[SLOT_OFF_BUILD_EPOCH], slot->build_epoch);
}

static void unpack_slot(const uint8_t in[SLOT_BLOCK_LEN], bootloader_slot_meta_t *slot)
{
    slot->state = (bootloader_slot_state_t)in[SLOT_OFF_STATE];
    slot->length = get_u32_le(&in[SLOT_OFF_LENGTH]);
    slot->crc32 = get_u32_le(&in[SLOT_OFF_CRC32]);
    memcpy(slot->version, &in[SLOT_OFF_VERSION], sizeof(slot->version));
    memcpy(slot->build_commit, &in[SLOT_OFF_BUILD_COMMIT], sizeof(slot->build_commit));
    slot->build_epoch = get_u32_le(&in[SLOT_OFF_BUILD_EPOCH]);
}

void bootloader_metadata_pack(const bootloader_metadata_t *meta,
                               uint8_t out[BOOTLOADER_METADATA_RECORD_LEN])
{
    memset(out, 0xFF, BOOTLOADER_METADATA_RECORD_LEN); // matches erased-flash background
    put_u32_le(&out[REC_OFF_MAGIC], BOOTLOADER_METADATA_MAGIC);
    put_u16_le(&out[REC_OFF_FORMAT_VERSION], meta->format_version);
    put_u16_le(&out[6], 0); // reserved0
    put_u32_le(&out[REC_OFF_SEQ], meta->seq);
    out[REC_OFF_ACTIVE_SLOT] = meta->active_slot;
    out[REC_OFF_BOOT_ATTEMPTS] = meta->boot_attempts;
    put_u16_le(&out[14], 0); // reserved1
    pack_slot(&meta->slots[BOOTLOADER_SLOT_A], &out[REC_OFF_SLOTS]);
    pack_slot(&meta->slots[BOOTLOADER_SLOT_B], &out[REC_OFF_SLOTS + SLOT_BLOCK_LEN]);
    // bytes [120, 252) already 0xFF from the initial memset -- reserved.
    uint32_t crc = bootloader_crc32(out, REC_OFF_CRC);
    put_u32_le(&out[REC_OFF_CRC], crc);
}

bool bootloader_metadata_unpack(const uint8_t in[BOOTLOADER_METADATA_RECORD_LEN],
                                 bootloader_metadata_t *out)
{
    if (in == NULL || out == NULL) {
        return false;
    }

    if (get_u32_le(&in[REC_OFF_MAGIC]) != BOOTLOADER_METADATA_MAGIC) {
        return false; // erased flash (0xFFFFFFFF) or garbage -- not this format
    }
    if (get_u16_le(&in[REC_OFF_FORMAT_VERSION]) != BOOTLOADER_METADATA_FORMAT_VERSION) {
        return false; // refuse the unrecognised rather than guess (docs/BOOTLOADER.md section 2)
    }

    uint32_t stored_crc = get_u32_le(&in[REC_OFF_CRC]);
    uint32_t computed_crc = bootloader_crc32(in, REC_OFF_CRC);
    if (stored_crc != computed_crc) {
        return false; // corrupted, or a torn write caught mid-program
    }

    // Well-formed -- only now do we touch *out.
    out->format_version = get_u16_le(&in[REC_OFF_FORMAT_VERSION]);
    out->seq = get_u32_le(&in[REC_OFF_SEQ]);
    out->active_slot = in[REC_OFF_ACTIVE_SLOT];
    out->boot_attempts = in[REC_OFF_BOOT_ATTEMPTS];
    unpack_slot(&in[REC_OFF_SLOTS], &out->slots[BOOTLOADER_SLOT_A]);
    unpack_slot(&in[REC_OFF_SLOTS + SLOT_BLOCK_LEN], &out->slots[BOOTLOADER_SLOT_B]);
    return true;
}

size_t bootloader_metadata_find_latest(const uint8_t sector[BOOTLOADER_METADATA_FLASH_SIZE],
                                        bootloader_metadata_t *out_meta)
{
    if (sector == NULL || out_meta == NULL) {
        return BOOTLOADER_METADATA_NO_SLOT;
    }

    size_t best_slot = BOOTLOADER_METADATA_NO_SLOT;
    bootloader_metadata_t best_meta;
    memset(&best_meta, 0, sizeof(best_meta));
    bool have_best = false;

    for (size_t i = 0; i < BOOTLOADER_METADATA_SLOTS_PER_SECTOR; i++) {
        const uint8_t *rec = &sector[i * BOOTLOADER_METADATA_RECORD_LEN];
        bootloader_metadata_t candidate;
        if (!bootloader_metadata_unpack(rec, &candidate)) {
            continue; // erased or corrupt -- skip, not an error
        }
        if (!have_best || candidate.seq > best_meta.seq) {
            best_meta = candidate;
            best_slot = i;
            have_best = true;
        }
    }

    if (!have_best) {
        return BOOTLOADER_METADATA_NO_SLOT;
    }

    *out_meta = best_meta;
    return best_slot;
}

size_t bootloader_metadata_next_write_slot(size_t latest_slot_index)
{
    if (latest_slot_index == BOOTLOADER_METADATA_NO_SLOT) {
        return 0;
    }
    size_t next = latest_slot_index + 1;
    if (next >= BOOTLOADER_METADATA_SLOTS_PER_SECTOR) {
        return 0;
    }
    return next;
}

bool bootloader_metadata_next_write_needs_erase(size_t latest_slot_index)
{
    if (latest_slot_index == BOOTLOADER_METADATA_NO_SLOT) {
        return false; // never written -- slot 0 may already be erased, don't assume otherwise
    }
    return (latest_slot_index + 1) >= BOOTLOADER_METADATA_SLOTS_PER_SECTOR;
}

// --- Boot decision -----------------------------------------------------

static bool slot_is_bootable(bootloader_slot_state_t state)
{
    return state == BOOTLOADER_SLOT_VALID || state == BOOTLOADER_SLOT_PENDING_VERIFY;
}

static bootloader_boot_decision_t decision_not_bootable(const bootloader_metadata_t *meta)
{
    bootloader_boot_decision_t d;
    d.bootable = false;
    d.chosen_slot = BOOTLOADER_SLOT_A;
    d.needs_metadata_update = false;
    d.updated_meta = *meta;
    return d;
}

static bootloader_boot_decision_t decision_boot(const bootloader_metadata_t *meta,
                                                  uint8_t chosen, bool switched,
                                                  uint8_t new_boot_attempts)
{
    bootloader_boot_decision_t d;
    d.bootable = true;
    d.chosen_slot = chosen;
    d.needs_metadata_update = true;
    d.updated_meta = *meta;
    if (switched) {
        d.updated_meta.active_slot = chosen;
    }
    d.updated_meta.boot_attempts = new_boot_attempts;
    return d;
}

bootloader_boot_decision_t bootloader_decide_boot(const bootloader_metadata_t *meta)
{
    uint8_t active = meta->active_slot;
    uint8_t other = (active == BOOTLOADER_SLOT_A) ? BOOTLOADER_SLOT_B : BOOTLOADER_SLOT_A;

    if (meta->boot_attempts >= BOOTLOADER_MAX_BOOT_ATTEMPTS) {
        // docs/BOOTLOADER.md section 3 step 3: mark BAD, switch if the other
        // slot is usable.
        bootloader_metadata_t marked = *meta;
        marked.slots[active].state = BOOTLOADER_SLOT_BAD;

        if (slot_is_bootable(marked.slots[other].state)) {
            return decision_boot(&marked, other, /*switched=*/true, /*new_boot_attempts=*/1);
        }

        bootloader_boot_decision_t d = decision_not_bootable(&marked);
        d.needs_metadata_update = true; // the BAD marking must still persist
        return d;
    }

    if (slot_is_bootable(meta->slots[active].state)) {
        return decision_boot(meta, active, /*switched=*/false,
                              (uint8_t)(meta->boot_attempts + 1u));
    }

    // Active slot is not currently bootable (EMPTY/STAGED/BAD) but hasn't
    // exhausted its attempt limit -- that limit only matters for a slot that
    // WAS bootable and kept failing to confirm. An EMPTY/STAGED/BAD slot is
    // unbootable right now regardless, so fall back immediately.
    if (slot_is_bootable(meta->slots[other].state)) {
        return decision_boot(meta, other, /*switched=*/true, /*new_boot_attempts=*/1);
    }

    return decision_not_bootable(meta);
}

bootloader_boot_decision_t bootloader_decide_after_crc_fail(const bootloader_metadata_t *meta,
                                                              uint8_t failed_slot)
{
    uint8_t other = (failed_slot == BOOTLOADER_SLOT_A) ? BOOTLOADER_SLOT_B : BOOTLOADER_SLOT_A;

    bootloader_metadata_t marked = *meta;
    marked.slots[failed_slot].state = BOOTLOADER_SLOT_BAD;

    if (slot_is_bootable(marked.slots[other].state)) {
        return decision_boot(&marked, other, /*switched=*/true, /*new_boot_attempts=*/1);
    }

    bootloader_boot_decision_t d = decision_not_bootable(&marked);
    d.needs_metadata_update = true; // the BAD marking must still persist
    return d;
}
