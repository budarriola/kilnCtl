// update_task_metadata_write.h -- the erase / erased-check / program /
// byte-for-byte read-back sequence behind update_task_persist_metadata().
//
// UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09 L4: the metadata write checked
// only the HAL return codes. A program onto a non-erased slot silently
// becomes (existing & new), and a "successful" program can still leave wrong
// bytes; either would report COMPLETE. This mirrors config_store_flash.c:
// refuse to program a slot that is not provably erased, and read the record
// back and compare it to the intended bytes. Any disagreement is a failure.
//
// Pure (I/O through callbacks) so it is host-testable; update_task.c cannot
// be host-compiled.
#ifndef SAFTYFW_TASKS_UPDATE_TASK_METADATA_WRITE_H
#define SAFTYFW_TASKS_UPDATE_TASK_METADATA_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPDATE_METADATA_WRITE_OK = 0,
    UPDATE_METADATA_WRITE_ERASE_FAILED,
    UPDATE_METADATA_WRITE_SLOT_NOT_ERASED, // refused: would AND-corrupt
    UPDATE_METADATA_WRITE_PROGRAM_FAILED,
    UPDATE_METADATA_WRITE_READBACK_MISMATCH,
} update_metadata_write_result_t;

// All callbacks return true on success. `offset` is the byte offset within
// the metadata sector.
typedef struct {
    bool (*erase)(void *ctx);
    bool (*read)(void *ctx, uint32_t offset, uint8_t *buf, size_t len);
    bool (*program)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    void *ctx;
} update_metadata_write_io_t;

update_metadata_write_result_t update_task_metadata_write_verified(
    const update_metadata_write_io_t *io, bool needs_erase, uint32_t slot_offset,
    const uint8_t *record, size_t record_len);

#ifdef __cplusplus
}
#endif
#endif
