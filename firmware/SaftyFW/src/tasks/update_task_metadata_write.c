#include "tasks/update_task_metadata_write.h"

#include <string.h>

#define UPDATE_METADATA_MAX_RECORD 256u

update_metadata_write_result_t update_task_metadata_write_verified(
    const update_metadata_write_io_t *io, bool needs_erase, uint32_t slot_offset,
    const uint8_t *record, size_t record_len)
{
    uint8_t buf[UPDATE_METADATA_MAX_RECORD];
    if (io == NULL || record == NULL || record_len == 0u || record_len > sizeof(buf)) {
        return UPDATE_METADATA_WRITE_PROGRAM_FAILED;
    }
    if (needs_erase && !io->erase(io->ctx)) {
        return UPDATE_METADATA_WRITE_ERASE_FAILED;
    }
    // Erased check: an unreadable slot is not provably erased (fail closed).
    if (!io->read(io->ctx, slot_offset, buf, record_len)) {
        return UPDATE_METADATA_WRITE_SLOT_NOT_ERASED;
    }
    for (size_t i = 0; i < record_len; i++) {
        if (buf[i] != 0xFFu) {
            return UPDATE_METADATA_WRITE_SLOT_NOT_ERASED;
        }
    }
    if (!io->program(io->ctx, slot_offset, record, record_len)) {
        return UPDATE_METADATA_WRITE_PROGRAM_FAILED;
    }
    if (!io->read(io->ctx, slot_offset, buf, record_len) ||
        memcmp(buf, record, record_len) != 0) {
        return UPDATE_METADATA_WRITE_READBACK_MISMATCH;
    }
    return UPDATE_METADATA_WRITE_OK;
}
