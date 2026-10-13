// test_update_task_metadata_write.c -- update_task_metadata_write_verified():
// erased-slot check and byte-for-byte read-back (audit L4,
// UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09). A fake sector models real flash
// AND-programming, with injectable corruption.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

#include "tasks/update_task_metadata_write.h"

#define REC 256u
typedef struct {
    uint8_t mem[REC * 4];
    bool fail_erase, fail_program, fail_read;
    bool corrupt_program;
    int erases, programs;
} fake_t;

static bool f_erase(void *c) { fake_t *f = c; f->erases++; if (f->fail_erase) return false; memset(f->mem, 0xFF, sizeof f->mem); return true; }
static bool f_read(void *c, uint32_t o, uint8_t *b, size_t n) { fake_t *f = c; if (f->fail_read) return false; memcpy(b, f->mem + o, n); return true; }
static bool f_prog(void *c, uint32_t o, const uint8_t *d, size_t n)
{
    fake_t *f = c; f->programs++;
    if (f->fail_program) return false;
    for (size_t i = 0; i < n; i++) f->mem[o + i] &= d[i];
    if (f->corrupt_program) f->mem[o + 7] ^= 0x01u;
    return true;
}

static update_metadata_write_result_t run(fake_t *f, bool erase, uint32_t slot, const uint8_t *rec)
{
    update_metadata_write_io_t io = { f_erase, f_read, f_prog, f };
    return update_task_metadata_write_verified(&io, erase, slot * REC, rec, REC);
}

void run_test_update_task_metadata_write(void)
{
    uint8_t rec[REC];
    for (size_t i = 0; i < REC; i++) rec[i] = (uint8_t)(i * 7u + 1u);
    fake_t f;

    TEST_SECTION("update_task_metadata_write_verified");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    TEST_CHECK(run(&f, false, 1, rec) == UPDATE_METADATA_WRITE_OK && memcmp(f.mem + REC, rec, REC) == 0,
               "clean write to erased slot succeeds and lands the bytes");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.mem[REC + 3] = 0x00;
    TEST_CHECK(run(&f, false, 1, rec) == UPDATE_METADATA_WRITE_SLOT_NOT_ERASED && f.programs == 0,
               "non-erased slot is refused before any program");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.mem[0] = 0x12;
    TEST_CHECK(run(&f, true, 0, rec) == UPDATE_METADATA_WRITE_OK && f.erases == 1,
               "wrapped write erases first, then the erased check passes");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.fail_erase = true;
    TEST_CHECK(run(&f, true, 0, rec) == UPDATE_METADATA_WRITE_ERASE_FAILED && f.programs == 0,
               "failed erase never programs");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.fail_program = true;
    TEST_CHECK(run(&f, false, 1, rec) == UPDATE_METADATA_WRITE_PROGRAM_FAILED, "failed program reported");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.corrupt_program = true;
    TEST_CHECK(run(&f, false, 1, rec) == UPDATE_METADATA_WRITE_READBACK_MISMATCH,
               "program that returns ok but lands wrong bytes fails the read-back");

    memset(&f, 0xFF, sizeof f); memset(&f.fail_erase, 0, sizeof f - sizeof f.mem);
    f.fail_read = true;
    TEST_CHECK(run(&f, false, 1, rec) == UPDATE_METADATA_WRITE_SLOT_NOT_ERASED && f.programs == 0,
               "unreadable slot is not provably erased: fail closed");
}
